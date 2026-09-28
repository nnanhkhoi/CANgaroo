#pragma once

#include "core/BusMessage.h"
#include <chrono>
#include <map>
#include <optional>

// One instance per live interface. Passive traces never drive the bus.
// Recognize normal-addressed diagnostic replies to our own physical requests.
class IsotpFlowControl
{
public:
    using Clock = std::chrono::steady_clock;

    std::optional<BusMessage> process(const BusMessage &frame,
                                     Clock::time_point now = Clock::now())
    {
        for (auto it = m_pending.begin(); it != m_pending.end(); )
            if (now >= it->second.expires) it = m_pending.erase(it);
            else ++it;

        if (frame.busType() != BusType::CAN || frame.isRTR() || frame.isErrorFrame()
            || frame.getLength() < 3 || frame.isFD())
            return std::nullopt;

        const auto id = frame.getId();
        const auto key = (uint64_t(frame.getInterfaceId()) << 32)
            | (frame.isExtended() ? 0x80000000u : 0u) | id;
        const auto pci = frame.getByte(0) >> 4;
        if (!frame.isRX())
        {
            // Only a complete single-frame UDS request arms a reply slot.
            const auto size = frame.getByte(0) & 15;
            const auto sid = frame.getByte(1);
            if (pci != 0 || size == 0 || size + 1 > frame.getLength()
                || sid < 0x10 || sid >= 0x40)
                return std::nullopt;
            uint32_t responseId;
            if (!frame.isExtended() && id >= 0x7E0 && id <= 0x7E7)
                responseId = id + 8;
            else if (frame.isExtended() && (id & 0x1FFF0000u) == 0x18DA0000u)
                responseId = (id & 0x1FFF0000u) | ((id & 255) << 8) | ((id >> 8) & 255);
            else
                return std::nullopt;
            m_pending[(key & 0xFFFFFFFF00000000ull)
                | (frame.isExtended() ? 0x80000000u : 0u) | responseId]
                = {id, sid, now + std::chrono::seconds(5)};
            return std::nullopt;
        }

        auto it = m_pending.find(key);
        if (it == m_pending.end()) return std::nullopt;
        if (pci == 0)
        {
            const auto size = frame.getByte(0) & 15;
            if (size == 0 || size + 1 > frame.getLength()) return std::nullopt;
            if (size >= 3 && frame.getByte(1) == 0x7F && frame.getByte(2) == it->second.sid
                && frame.getByte(3) == 0x78)
                it->second.expires = now + std::chrono::seconds(5);
            else if (frame.getByte(1) == it->second.sid + 0x40
                     || (size >= 3 && frame.getByte(1) == 0x7F && frame.getByte(2) == it->second.sid))
                m_pending.erase(it);
            return std::nullopt;
        }
        const int size = ((frame.getByte(0) & 15) << 8) | frame.getByte(1);
        if (pci != 1 || frame.getLength() != 8 || size <= 7
            || frame.getByte(2) != it->second.sid + 0x40)
            return std::nullopt;

        BusMessage fc(it->second.txId);
        fc.setInterfaceId(frame.getInterfaceId());
        fc.setExtended(frame.isExtended());
        fc.setRX(false);
        fc.setLength(8);
        // Continue to send, unlimited block size, no additional separation time.
        fc.setData(0x30, 0, 0, 0, 0, 0, 0, 0);
        m_pending.erase(it);
        return fc;
    }

private:
    struct Pending
    {
        uint32_t txId;
        uint8_t sid;
        Clock::time_point expires;
    };
    std::map<uint64_t, Pending> m_pending;
};
