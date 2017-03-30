/* Copyright 2009-2015 Pierre Ossman for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

/*
 * This code implements congestion control in the same way as TCP in
 * order to avoid excessive latency in the transport. This is needed
 * because "buffer bloat" is unfortunately still a very real problem.
 *
 * The basic principle is TCP Congestion Control (RFC 5618), with the
 * addition of using the TCP Vegas algorithm. The reason we use Vegas
 * is that we run on top of a reliable transport so we need a latency
 * based algorithm rather than a loss based one. There is also a lot of
 * interpolation of values. This is because we have rather horrible
 * granularity in our measurements.
 */

#include <assert.h>
#include <sys/time.h>

#include <rfb/Congestion.h>
#include <rfb/LogWriter.h>
#include <rfb/util.h>

// Debug output on what the congestion control is up to
#undef CONGESTION_DEBUG

using namespace rfb;

// This window should get us going fairly fast on a decent bandwidth network.
// If it's too high, it will rapidly be reduced and stay low.
static const unsigned INITIAL_WINDOW = 16384;

// TCP's minimal window is 3*MSS. But since we don't know the MSS, we
// make a guess at 4 KiB (it's probably a bit higher).
static const unsigned MINIMUM_WINDOW = 4096;

// The current default maximum window for Linux (4 MiB). Should be a good
// limit for now...
static const unsigned MAXIMUM_WINDOW = 4194304;

static LogWriter vlog("Congestion");

Congestion::Congestion() :
    lastPosition(0), extraBuffer(0),
    baseRTT(-1), congWindow(INITIAL_WINDOW),
    measurements(0), minRTT(-1), minCongestedRTT(-1)
{
  gettimeofday(&lastUpdate, NULL);
  gettimeofday(&lastSent, NULL);
  memset(&lastPong, 0, sizeof(lastPong));
  gettimeofday(&lastPongArrival, NULL);
  gettimeofday(&lastAdjustment, NULL);
}

Congestion::~Congestion()
{
}

void Congestion::updatePosition(unsigned pos)
{
  struct timeval now;
  unsigned delta, consumed;

  gettimeofday(&now, NULL);

  delta = pos - lastPosition;
  if ((delta > 0) || (extraBuffer > 0))
    lastSent = now;

  // Idle for too long?
  // We use a very crude RTO calculation in order to keep things simple
  // FIXME: should implement RFC 2861
  if (msBetween(&lastSent, &now) > __rfbmax(baseRTT*2, 100)) {

#ifdef CONGESTION_DEBUG
    vlog.debug("Connection idle for %d ms, resetting congestion control",
               msBetween(&lastSent, &now));
#endif

    // Close congestion window and redo wire latency measurement
    congWindow = __rfbmin(INITIAL_WINDOW, congWindow);
    baseRTT = -1;
    measurements = 0;
    gettimeofday(&lastAdjustment, NULL);
    minRTT = minCongestedRTT = -1;
  }

  // Commonly we will be in a state of overbuffering. We need to
  // estimate the extra delay that causes so we can separate it from
  // the delay caused by an incorrect congestion window.
  // (we cannot do this until we have a RTT measurement though)
  if (baseRTT != (unsigned)-1) {
    extraBuffer += delta;
    consumed = msBetween(&lastUpdate, &now) * congWindow / baseRTT;
    if (extraBuffer < consumed)
      extraBuffer = 0;
    else
      extraBuffer -= consumed;
  }

  lastPosition = pos;
  lastUpdate = now;
}

void Congestion::sentPing()
{
  struct RTTInfo rttInfo;

  memset(&rttInfo, 0, sizeof(struct RTTInfo));

  gettimeofday(&rttInfo.tv, NULL);
  rttInfo.pos = lastPosition;
  rttInfo.extra = getExtraBuffer();
  rttInfo.congested = isCongested();

  pings.push_back(rttInfo);
}

void Congestion::gotPong()
{
  struct timeval now;
  struct RTTInfo rttInfo;
  unsigned rtt, delay;

  if (pings.empty())
    return;

  gettimeofday(&now, NULL);

  rttInfo = pings.front();
  pings.pop_front();

  lastPong = rttInfo;
  lastPongArrival = now;

  rtt = msBetween(&rttInfo.tv, &now);
  if (rtt < 1)
    rtt = 1;

  // Try to estimate wire latency by tracking lowest seen latency
  if (rtt < baseRTT)
    baseRTT = rtt;

  // Pings sent before the last adjustment aren't interesting as they
  // aren't a measurement of the current congestion window
  if (isBefore(&rttInfo.tv, &lastAdjustment))
    return;

  // Estimate added delay because of overtaxed buffers (see above)
  delay = rttInfo.extra * baseRTT / congWindow;
  if (delay < rtt)
    rtt -= delay;
  else
    rtt = 1;

  // A latency less than the wire latency means that we've
  // understimated the congestion window. We can't really determine
  // how much, so pretend that we got no buffer latency at all.
  if (rtt < baseRTT)
    rtt = baseRTT;

  // Record the minimum seen delay (hopefully ignores jitter) and let
  // the congestion control do its thing.
  //
  // Note: We are delay based rather than loss based, which means we
  //       need to look at pongs even if they weren't limited by the
  //       current window ("congested"). Otherwise we will fail to
  //       detect increasing congestion until the application exceeds
  //       the congestion window.
  if (rtt < minRTT)
    minRTT = rtt;
  if (rttInfo.congested) {
    if (rtt < minCongestedRTT)
      minCongestedRTT = rtt;
  }

  measurements++;
  updateCongestion();
}

bool Congestion::isCongested()
{
  if (getInFlight() < congWindow)
    return false;

  return true;
}

int Congestion::getUncongestedETA()
{
  unsigned targetAcked;

  const struct RTTInfo* prevPing;
  unsigned eta, elapsed;
  unsigned etaNext, delay;

  std::list<struct RTTInfo>::const_iterator iter;

  targetAcked = lastPosition - congWindow;

  // Simple case?
  if (lastPong.pos > targetAcked)
    return 0;

  // No measurements yet?
  if (baseRTT == (unsigned)-1)
    return -1;

  prevPing = &lastPong;
  eta = 0;
  elapsed = msSince(&lastPongArrival);

  // Walk the ping queue and figure out which one we are waiting for to
  // get to an uncongested state

  for (iter = pings.begin();iter != pings.end();++iter) {
    etaNext = msBetween(&prevPing->tv, &iter->tv);
    // Compensate for buffering delays
    delay = iter->extra * baseRTT / congWindow;
    etaNext += delay;
    delay = prevPing->extra * baseRTT / congWindow;
    if (delay >= etaNext)
      etaNext = 0;
    else
      etaNext -= delay;

    // Found it?
    if (iter->pos > targetAcked) {
      eta += etaNext * (iter->pos - targetAcked) / (iter->pos - prevPing->pos);
      if (elapsed > eta)
        return 0;
      else
        return eta - elapsed;
    }

    eta += etaNext;
    prevPing = &*iter;
  }

  // We aren't waiting for a pong that will clear the congested state.
  // Estimate the final bit by pretending that we had a ping just after
  // the last position update.

  etaNext = msBetween(&prevPing->tv, &lastUpdate);
  delay = extraBuffer * baseRTT / congWindow;
  etaNext += delay;
  delay = prevPing->extra * baseRTT / congWindow;
  if (delay >= etaNext)
    etaNext = 0;
  else
    etaNext -= delay;

  eta += etaNext * (lastPosition - targetAcked) / (lastPosition - prevPing->pos);
  if (elapsed > eta)
    return 0;
  else
    return eta - elapsed;
}

unsigned Congestion::getExtraBuffer()
{
  unsigned elapsed;
  unsigned consumed;

  if (baseRTT == (unsigned)-1)
    return 0;

  elapsed = msSince(&lastUpdate);
  consumed = elapsed * congWindow / baseRTT;

  if (consumed >= extraBuffer)
    return 0;
  else
    return extraBuffer - consumed;
}

unsigned Congestion::getInFlight()
{
  unsigned acked;

  // Simple case?
  if (lastPosition == lastPong.pos)
    return 0;

  // No measurements yet?
  if (baseRTT == (unsigned)-1) {
    if (!pings.empty())
      return lastPosition - pings.front().pos;
    return 0;
  }

  // First we need to estimate how many bytes have made it through
  // completely
  if (!pings.empty()) {
    struct RTTInfo nextPong;
    unsigned etaNext, delay, elapsed;

    // There is at least one more ping that should arrive. Figure out
    // how far behind it should be and interpolate the positions.

    nextPong = pings.front();

    etaNext = msBetween(&lastPong.tv, &nextPong.tv);
    // Compensate for buffering delays
    delay = nextPong.extra * baseRTT / congWindow;
    etaNext += delay;
    delay = lastPong.extra * baseRTT / congWindow;
    if (delay >= etaNext)
      etaNext = 0;
    else
      etaNext -= delay;

    elapsed = msSince(&lastPongArrival);

    // The pong should be here any second. Be optimistic and assume
    // we can already use its value.
    if (etaNext <= elapsed)
      acked = nextPong.pos;
    else {
      acked = lastPong.pos;
      acked += (nextPong.pos - lastPong.pos) * elapsed / etaNext;
    }
  } else {
    unsigned elapsed;

    // We are not waiting for any pongs so we're just going to have to
    // guess based on how much time since the last position update.

    elapsed = msSince(&lastUpdate);
    if (elapsed <= baseRTT)
      acked = 0;
    else
      acked = (elapsed - baseRTT) * congWindow / baseRTT;

    if (acked > extraBuffer)
      acked = extraBuffer;

    acked = (lastPosition - extraBuffer) + acked;
  }

  return lastPosition - acked;
}

void Congestion::updateCongestion()
{
  unsigned diff;

  // We want at least three measurements to avoid noise
  if (measurements < 3)
    return;

  assert(minRTT >= baseRTT);
  assert(minCongestedRTT >= baseRTT);

  // The goal is to have a slightly too large congestion window since
  // a "perfect" one cannot be distinguished from a too small one. This
  // translates to a goal of a few extra milliseconds of delay.

  // First we check all pongs to make sure we're not having a too large
  // congestion window.
  diff = minRTT - baseRTT;

  // FIXME: Should we do slow start?
  if (diff > 100) {
    // Way too fast
    congWindow = congWindow * baseRTT / minRTT;
  } else if (diff > 50) {
    // Slightly too fast
    congWindow -= 4096;
  } else {
    // Secondly only the "congested" pongs are checked to see if the
    // window is too small.

    diff = minCongestedRTT - baseRTT;

    if (diff < 5) {
      // Way too slow
      congWindow += 8192;
    } else if (diff < 25) {
      // Too slow
      congWindow += 4096;
    }
  }

  if (congWindow < MINIMUM_WINDOW)
    congWindow = MINIMUM_WINDOW;
  if (congWindow > MAXIMUM_WINDOW)
    congWindow = MAXIMUM_WINDOW;

#ifdef CONGESTION_DEBUG
  vlog.debug("RTT: %d ms (%d ms), Window: %d KiB, Bandwidth: %g Mbps",
             minRTT, baseRTT, congWindow / 1024,
             congWindow * 8.0 / baseRTT / 1000.0);
#endif

  measurements = 0;
  gettimeofday(&lastAdjustment, NULL);
  minRTT = minCongestedRTT = -1;
}

