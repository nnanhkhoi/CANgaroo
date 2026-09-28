# ISO-TP reception in CANgaroo

During a live measurement, CANgaroo automatically sends a Flow Control frame
for a multi-frame positive UDS response to a physical single-frame request it
has transmitted. This runs in the interface listener, independently of which
trace tab is open.

Supported automatic reply mappings (Classical CAN, normal addressing):

- Request IDs `0x7E0`–`0x7E7`, response ID = request ID + 8.
- 29-bit `0x18DA<TA><SA>` requests, with source and target swapped in the reply.

For example, TX `0x7E0: 03 22 F1 87 00 00 00 00` followed by RX
`0x7E8: 10 0C 62 F1 87 DE AD AD` produces TX
`0x7E0: 30 00 00 00 00 00 00 00`. The peer can then send the remaining CF.
The FC uses block size 0 and STmin 0; no further FC is required for that response.

The request slot expires after five seconds. A matching negative response with
NRC `0x78` refreshes that timeout. A completed single-frame response closes it.
Unsolicited traffic, imported traces, and passive monitoring do not trigger FC.

The UDS Protocol tab displays accepted FF and intermediate CF as transport rows
immediately, even if the peer never finishes its response. Standard diagnostic
FC frames are also visible there. Once reassembly completes, a decoded UDS row
contains all its original FF/CF as children; the earlier transport rows remain
as a record of arrival. Monitor continues to show every physical frame.

This is automatic reception of diagnostic responses, not a complete ISO-TP
sender. Segmenting outgoing payloads, functional requests, custom CAN ID pairs,
extended/mixed addressing, and automatic FC for CAN FD are not implemented.
