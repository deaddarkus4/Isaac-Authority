"""One direction of an impaired network path for the live tests: fixed delay, uniform jitter, independent loss.

Packets leave in the order of their release times, so jitter reorders them the way a real path does. The modules compare a
packet's timestamp with their own clock (250 ms), which only works while both games share one machine; the whole delay has
to stay inside that window."""
import heapq
import random

MAX_TOTAL_DELAY_MS = 200


class Link:
    def __init__(self, deliver, delay_ms=0, jitter_ms=0, loss_percent=0.0, seed=1):
        if delay_ms < 0 or jitter_ms < 0 or delay_ms + jitter_ms > MAX_TOTAL_DELAY_MS or not 0 <= loss_percent < 100:
            raise ValueError("Delay plus jitter must stay within %d ms and loss below 100 %%" % MAX_TOTAL_DELAY_MS)
        self.deliver, self.delay_ms, self.jitter_ms, self.loss = deliver, delay_ms, jitter_ms, loss_percent / 100.0
        self.random = random.Random(seed)
        self.queue, self.order = [], 0
        self.offered = self.dropped = self.delivered = self.reordered = 0
        self.newest = -1

    def send(self, packet, now_ms, tag=None):
        """Offer a packet at now_ms; tag travels with it to the deliver callback. False when the path lost it."""
        self.offered += 1
        if self.random.random() < self.loss:
            self.dropped += 1
            return False
        release = now_ms + self.delay_ms + (self.random.uniform(0, self.jitter_ms) if self.jitter_ms else 0)
        heapq.heappush(self.queue, (release, self.order, packet, tag)); self.order += 1
        return True

    def flush(self, now_ms):
        """Deliver everything whose time has come; returns how many packets left the path."""
        count = 0
        while self.queue and self.queue[0][0] <= now_ms:
            _, order, packet, tag = heapq.heappop(self.queue)
            if order < self.newest:
                self.reordered += 1
            self.newest = max(self.newest, order)
            self.deliver(packet, tag); self.delivered += 1; count += 1
        return count

    def report(self):
        return dict(delayMs=self.delay_ms, jitterMs=self.jitter_ms, lossPercent=round(self.loss * 100, 3), offered=self.offered,
                    dropped=self.dropped, delivered=self.delivered, reordered=self.reordered, inFlightAtEnd=len(self.queue))
