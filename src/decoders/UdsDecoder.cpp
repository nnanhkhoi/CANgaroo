#include "UdsDecoder.h"

UdsDecoder::UdsDecoder() {}

static constexpr uint32_t k_extendedSessionFlag = 0x80000000u;
static constexpr uint32_t k_txSessionFlag       = 0x40000000u;

// RX and TX traffic on the same ID get independent session slots. Without
// this, an interface that loops transmitted frames back on the RX path
// (SocketCAN/vcan, some real adapters) delivers every sent frame twice, and
// a duplicate consecutive frame trips the strict sequence-number check below
// and kills the session. Keying by direction also lets CANgaroo decode a UDS
// request it transmits itself (no RX counterpart exists for that direction
// when talking to a real external device) independently of whatever it
// receives back.
//
// The interface/channel is folded in too: diagnostic IDs (0x7E0/0x7E8,
// 0x18DAxxyy, ...) are commonly reused across separate buses, so without a
// per-channel key an interleaved frame from a second channel would look
// like a sequence-number violation and kill the first channel's session.
static uint64_t sessionKey(uint32_t id, bool extended, bool isRX, uint16_t interfaceId) noexcept
{
    uint32_t key32 = extended ? (id | k_extendedSessionFlag) : id;
    if (!isRX) key32 |= k_txSessionFlag;
    return (static_cast<uint64_t>(interfaceId) << 32) | key32;
}

// UDS service IDs begin at 0x10. Anything below that cannot be a valid SID.
static bool isValidUdsSid(uint8_t sid) noexcept
{
    return sid >= 0x10;
}

static void fillOutMsg(ProtocolMessage& outMsg, uint8_t sid, const QString& name,
                       const QByteArray& payload, const QVector<BusMessage>& frames,
                       uint32_t canId, bool extended)
{
    outMsg.payload  = payload;
    outMsg.rawFrames = frames;
    outMsg.protocol = "uds";
    outMsg.timestamp = static_cast<uint64_t>(frames.first().getFloatTimestamp() * 1000000.0);
    outMsg.id   = sid;
    outMsg.name = name;

    if (extended) {
        // ISO 15765-4 extended addressing: SA in the low byte, target
        // address (DA) in the PS byte of the 29-bit identifier.
        outMsg.metadata["Source Address"] = canId & 0xFF;
        outMsg.metadata["Target Address"] = (canId >> 8) & 0xFF;
    }
}

DecodeStatus UdsDecoder::tryDecode(const BusMessage& frame, ProtocolMessage& outMsg) {
    if (frame.isErrorFrame() || frame.isRTR() || frame.getLength() < 1) {
        return DecodeStatus::Ignored;
    }

    uint8_t type = (frame.getByte(0) >> 4) & 0x0F;
    uint32_t id  = frame.getId();
    uint64_t key = sessionKey(id, frame.isExtended(), frame.isRX(), frame.getInterfaceId());

    if (frame.isExtended()) {
        uint8_t pf = (id >> 16) & 0xFF;
        if (pf == 0xEC || pf == 0xEB) { // J1939 TP.CM or TP.DT
            return DecodeStatus::Ignored;
        }
    }

    if (type == 0) { // Single Frame
        int size      = frame.getByte(0) & 0x0F;
        int dataOffset = 1;

        // ISO 15765-2:2016 — CAN FD long single frame: size nibble == 0
        if (size == 0 && frame.getLength() > 1) {
            size       = frame.getByte(1);
            dataOffset = 2;
        }

        if (size > 0 && dataOffset + size <= frame.getLength()) {
            uint8_t sid = frame.getByte(dataOffset);
            if (!isValidUdsSid(sid))
                return DecodeStatus::Ignored;

            QByteArray payload;
            for (int i = 0; i < size; ++i)
                payload.append(frame.getByte(dataOffset + i));

            fillOutMsg(outMsg, sid, interpretService(sid), payload, {frame}, id, frame.isExtended());

            if (sid == 0x7F) {
                outMsg.type = MessageType::NegativeResponse;
                outMsg.description = (payload.size() >= 3)
                    ? QString("negative response: %1").arg(interpretNrc(static_cast<uint8_t>(payload.at(2))))
                    : "negative response: incomplete";
            } else if (sid & 0x40) {
                outMsg.type = MessageType::PositiveResponse;
                outMsg.description = QString("positive response to 0x%1").arg(sid - 0x40, 2, 16, QChar('0'));
            } else {
                outMsg.type = MessageType::Request;
                outMsg.description = QString("uds request, sid 0x%1").arg(sid, 2, 16, QChar('0'));
            }

            m_sessions.remove(key);
            return DecodeStatus::Completed;
        }
    } else if (type == 1) { // First Frame
        if (frame.getLength() < 3)
            return DecodeStatus::Ignored;
        int size       = ((frame.getByte(0) & 0x0F) << 8) | frame.getByte(1);
        int dataOffset = 2;

        // ISO 15765-2:2016 — CAN FD long first frame: 12-bit size field == 0
        if (size == 0 && frame.getLength() >= 6) {
            size = (static_cast<int>(frame.getByte(2)) << 24)
                 | (static_cast<int>(frame.getByte(3)) << 16)
                 | (static_cast<int>(frame.getByte(4)) <<  8)
                 |  static_cast<int>(frame.getByte(5));
            dataOffset = 6;
        }

        // Require at least the SID byte to be present before opening a session.
        if (dataOffset >= frame.getLength() || size <= frame.getLength() - dataOffset)
            return DecodeStatus::Ignored;

        if (!isValidUdsSid(frame.getByte(dataOffset)))
            return DecodeStatus::Ignored;

        IsotpSession& session = m_sessions[key];
        session.data = QByteArray();
        for (int i = dataOffset; i < frame.getLength(); ++i)
            session.data.append(frame.getByte(i));
        session.expectedSize = size;
        session.nextSn       = 1;
        session.frames       = {frame};
        session.rxId         = id;
        outMsg.protocol = "uds";
        return DecodeStatus::Consumed;

    } else if (type == 2) { // Consecutive Frame
        if (m_sessions.contains(key)) {
            IsotpSession& session = m_sessions[key];
            uint8_t sn = frame.getByte(0) & 0x0F;
            if (sn == session.nextSn) {
                session.frames.append(frame);
                for (int i = 1; i < frame.getLength() && session.data.size() < session.expectedSize; ++i)
                    session.data.append(frame.getByte(i));
                session.nextSn = (session.nextSn + 1) % 16;

                if (session.data.size() >= session.expectedSize) {
                    if (session.data.isEmpty()) {
                        m_sessions.remove(key);
                        return DecodeStatus::Ignored;
                    }
                    uint8_t sid = static_cast<uint8_t>(session.data.at(0));
                    fillOutMsg(outMsg, sid, interpretService(sid), session.data, session.frames, id, frame.isExtended());

                    if (sid == 0x7F) {
                        outMsg.type = MessageType::NegativeResponse;
                        outMsg.description = (session.data.size() >= 3)
                            ? QString("negative response: %1").arg(interpretNrc(static_cast<uint8_t>(session.data.at(2))))
                            : "negative response: incomplete";
                    } else if (sid & 0x40) {
                        outMsg.type = MessageType::PositiveResponse;
                        outMsg.description = QString("positive response to 0x%1").arg(sid - 0x40, 2, 16, QChar('0'));
                    } else {
                        outMsg.type = MessageType::Request;
                        outMsg.description = QString("uds request, sid 0x%1").arg(sid, 2, 16, QChar('0'));
                    }

                    m_sessions.remove(key);
                    return DecodeStatus::Completed;
                }
                outMsg.protocol = "uds";
                return DecodeStatus::Consumed;
            } else {
                m_sessions.remove(key);
                return DecodeStatus::Ignored;
            }
        }
    } else if (type == 3) { // Flow Control
        // Bug fix #1: only consume FC if an active session exists for this ID
        return m_sessions.contains(key) ? DecodeStatus::Consumed : DecodeStatus::Ignored;
    }

    return DecodeStatus::Ignored;
}

void UdsDecoder::reset() {
    m_sessions.clear();
}

QString UdsDecoder::interpretService(uint8_t sid) {
    if (sid == 0x7F) return "NegativeResponse";

    bool isResponse  = (sid & 0x40) && sid != 0x40; // 0x40 itself is not a UDS response
    uint8_t baseSid  = isResponse ? (sid - 0x40) : sid;

    switch (baseSid) {
        case 0x10: return "DiagnosticSessionControl";
        case 0x11: return "EcuReset";
        case 0x14: return "ClearDiagnosticInformation";
        case 0x19: return "ReadDTCInformation";
        case 0x22: return "ReadDataByIdentifier";
        case 0x23: return "ReadMemoryByAddress";
        case 0x27: return "SecurityAccess";
        case 0x28: return "CommunicationControl";
        case 0x29: return "Authentication";
        case 0x2A: return "ReadDataByPeriodicIdentifier";
        case 0x2C: return "DynamicallyDefineDataIdentifier";
        case 0x2E: return "WriteDataByIdentifier";
        case 0x2F: return "InputOutputControlByIdentifier";
        case 0x31: return "RoutineControl";
        case 0x34: return "RequestDownload";
        case 0x35: return "RequestUpload";
        case 0x36: return "TransferData";
        case 0x37: return "RequestTransferExit";
        case 0x38: return "RequestFileTransfer";
        case 0x3D: return "WriteMemoryByAddress";
        case 0x3E: return "TesterPresent";
        case 0x83: return "AccessTimingParameter";
        case 0x84: return "SecuredDataTransmission";
        case 0x85: return "ControlDTCSetting";
        case 0x86: return "ResponseOnEvent";
        case 0x87: return "LinkControl";
        default:   return QString("Service 0x%1").arg(sid, 2, 16, QChar('0'));
    }
}

QString UdsDecoder::interpretNrc(uint8_t nrc) {
    switch (nrc) {
        case 0x10: return "General Reject";
        case 0x11: return "Service Not Supported";
        case 0x12: return "SubFunction Not Supported";
        case 0x13: return "Incorrect Message Length Or Invalid Format";
        case 0x14: return "Response Too Long";
        case 0x21: return "Busy Repeat Request";
        case 0x22: return "Conditions Not Correct";
        case 0x24: return "Request Sequence Error";
        case 0x25: return "No Response From Subnet Component";
        case 0x26: return "Failure Prevents Execution Of Requested Action";
        case 0x29: return "Request Out Of Range (29)";
        case 0x31: return "Request Out Of Range";
        case 0x33: return "Security Access Denied";
        case 0x34: return "Authentication Required";
        case 0x35: return "Invalid Key";
        case 0x36: return "Exceed Number Of Attempts";
        case 0x37: return "Required Time Delay Not Expired";
        case 0x38: return "Secure Data Transmission Required";
        case 0x39: return "Secure Data Transmission Not Allowed";
        case 0x3A: return "Secure Data Verification Failed";
        case 0x50: return "Certificate Verification Failed - Invalid Time Period";
        case 0x51: return "Certificate Verification Failed - Invalid Signature";
        case 0x52: return "Certificate Verification Failed - Invalid Chain Of Trust";
        case 0x53: return "Certificate Verification Failed - Invalid Type";
        case 0x54: return "Certificate Verification Failed - Invalid Format";
        case 0x55: return "Certificate Verification Failed - Invalid Content";
        case 0x56: return "Certificate Verification Failed - Invalid Scope";
        case 0x57: return "Certificate Verification Failed - Invalid Certificate (Revoked)";
        case 0x58: return "Ownership Verification Failed";
        case 0x59: return "Challenge Calculation Failed";
        case 0x5A: return "Setting Access Rights Failed";
        case 0x5B: return "Session Key Creation/Derivation Failed";
        case 0x5C: return "Configuration Data Usage Failed";
        case 0x5D: return "DeAuthentication Failed";
        case 0x70: return "Upload Download Not Accepted";
        case 0x71: return "Transfer Data Suspended";
        case 0x72: return "General Programming Failure";
        case 0x73: return "Wrong Block Sequence Counter";
        case 0x78: return "Request Correctly Received - Response Pending";
        case 0x7E: return "SubFunction Not Supported In Active Session";
        case 0x7F: return "Service Not Supported In Active Session";
        case 0x81: return "Rpm Too High";
        case 0x82: return "Rpm Too Low";
        case 0x83: return "Engine Is Running";
        case 0x84: return "Engine Is Not Running";
        case 0x85: return "Engine Run Time Too Low";
        case 0x86: return "Temperature Too High";
        case 0x87: return "Temperature Too Low";
        case 0x88: return "Vehicle Speed Too High";
        case 0x89: return "Vehicle Speed Too Low";
        case 0x8A: return "Throttle Pedal Too High";
        case 0x8B: return "Throttle Pedal Too Low";
        case 0x8C: return "Transmission Range Not In Neutral";
        case 0x8D: return "Transmission Range Not In Gear";
        case 0x8F: return "Brake Switch Not Closed";
        case 0x90: return "Shifter Lever Not In Park";
        case 0x91: return "Torque Converter Clutch Locked";
        case 0x92: return "Voltage Too High";
        case 0x93: return "Voltage Too Low";
        default:   return QString("unknown (0x%1)").arg(nrc, 2, 16, QChar('0'));
    }
}
