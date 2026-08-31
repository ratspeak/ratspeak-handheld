#pragma once

#include "ServiceMailbox.h"

namespace handheld {

class ServiceHandler {
public:
    virtual ~ServiceHandler() = default;
    virtual void poll() = 0;
    // A handler either completes the reserved slot, or retains it while a
    // nonblocking operation is pending. Retained slots are never re-dispatched.
    virtual void execute(uint8_t slot) = 0;
};

class ServicePump {
public:
    ServicePump(ServiceMailbox& mailbox, ServiceHandler& handler)
        : _mailbox(mailbox), _handler(handler) {}
    void tick(uint32_t generation) {
        _handler.poll();
        for (unsigned count = 0; count < 2; ++count) {
            const auto slot = _mailbox.take();
            if (slot == ServiceMailbox::NoSlot) break;
            if (_mailbox.request(slot).generation != generation) {
                Result stale;
                stale.outcome = Outcome::Stale;
                _mailbox.complete(slot, stale);
                // A rejected stale lifecycle must not leave the owner closed.
                if (slot == ServiceMailbox::NormalSlots) _mailbox.setAccepting(true);
            } else {
                _handler.execute(slot);
            }
            // Timers and RX get a turn between potentially expensive commands.
            _handler.poll();
        }
    }
private:
    ServiceMailbox& _mailbox;
    ServiceHandler& _handler;
};

} // namespace handheld
