#pragma once

#include <QHostAddress>
#include <QNetworkInterface>
#include <QString>
#include <QVector>

// ---------------------------------------------------------------------------
// Feature flag for the per-PC connection type filter.
//
// Flip kEnabled to false to compile the ORIGINAL, unfiltered connection logic
// back in. Every decision point in the connection path is written like this:
//
//     if (NetLinkFilter::kEnabled && <filtered condition>) {
//         ...new filtered code...
//     } else {
//         ...original upstream code, unchanged...
//     }
//
// so changing this single constant restores the old behavior for the whole
// connection path. Three places are deliberately NOT gated, and none of them
// affect whether or how a host is reached:
//
//   - NvComputer::serialize() still skips writing `allowedlinktypes` while the
//     flag is false. Leaving the stored mask untouched is the point: a choice
//     the user made while the feature was live survives on disk and takes
//     effect again if the flag is flipped back, instead of being overwritten
//     with NLT_ALL and lost.
//   - ComputerManager::setHostConnectionOptions() stays reachable and warns
//     instead of returning silently, so a stray caller is visible rather than
//     writing a mask that nothing reads.
//   - The QML entry point is hidden through ComputerModel::netLinkFilterEnabled
//     rather than gated here, since QML cannot see a C++ constant.
//
// The in-memory allowedLinkTypes is still read from and validated against
// settings when the flag is false, so a stale or hand-edited value is sanitized
// on load exactly as it is when the feature is on.
// ---------------------------------------------------------------------------
namespace NetLinkFilter {
    constexpr bool kEnabled = true;
}

// Bit flags persisted in NvComputer::allowedLinkTypes. New entries must be
// appended at the end so existing user preferences keep their meaning.
enum NetLinkTypeBit : quint32
{
    NLT_WIRED    = 0x01,
    NLT_WIRELESS = 0x02,
    NLT_VIRTUAL  = 0x04,
    NLT_OTHER    = 0x08,
    NLT_ALL      = 0x0F
};

// One way this PC is able to reach a remote address.
//
// A multi-homed machine (for example a laptop with both Ethernet and WiFi on
// the same subnet) has several of these for a single remote address, which is
// why Moonlight cannot simply trust the first address it gets back from mDNS.
struct NetRoute
{
    QNetworkInterface::InterfaceType interfaceType = QNetworkInterface::Unknown;
    QString nicDescription;      // e.g. "Intel(R) Wi-Fi 6E AX211 160MHz"
    QHostAddress localAddress;   // The source address on this NIC
    int prefixLength = 0;
    bool onLink = false;         // The remote address is inside our subnet
    bool isPreferred = false;    // The OS would pick this route on its own

    NetLinkTypeBit bit() const;

    // A route is only usable if we actually found a source address for it
    bool isUsable() const { return !localAddress.isNull(); }
};

// Enumerate every route this PC has to `remoteAddress`, sorted best-first:
// on-link before gateway, wired before wireless, and the kernel's own choice
// ahead of the alternatives within a link type.
//
// Results are memoized briefly (the routing table does not change between
// polls, and every caller asks about the same few addresses over and over).
// The answer can therefore be up to one poll interval stale after the routing
// table changes - for a VPN coming up or a Wi-Fi roam, the filter may briefly
// allow an address it should not, or the reverse. Safe because the worst case
// is a host that is not reachable for one poll round, and because
// NvComputer::getConflictingLinkDescription() is the backstop that reports
// which adapter the OS actually chose.
//
// That bound covers an empty result too, which is the case that has to be
// included for it to be a bound at all: "no route to this address" is a
// statement about the routing table as it stands, and the table moves under
// us for reasons unrelated to the address. An empty result that outlived its
// TTL would strand the host until the process restarted, with no path back -
// allowedAddresses() drops the address, the poller stops probing, and the
// host can never learn that the link came back.
QVector<NetRoute> netRoutesTo(const QHostAddress& remoteAddress, uint16_t port);

// The highest ranked route whose connection type the user allows, or an
// unusable NetRoute when every route is excluded.
//
// This answers "is there any allowed way to reach the address?", not "is this
// the route the OS will use". Those differ when several adapters share a
// subnet: netRoutesTo()'s sort puts our own ranking ahead of the kernel's
// choice, and the kernel's choice is what carries the traffic. A usable result
// therefore means "allowed to try this address", never "this adapter will be
// used" - see NvComputer::getConflictingLinkDescription().
NetRoute bestAllowedRoute(const QVector<NetRoute>& routes, quint32 allowedBits);

// The bit for the route the OS would use to reach `remoteAddress`. Anything we
// cannot classify is reported as NLT_OTHER, so an unfamiliar NIC type never
// silently blocks a host.
NetLinkTypeBit netLinkTypeOf(const QHostAddress& remoteAddress, uint16_t port);

// Whether `address` is an IP literal that netRoutesTo() is able to classify.
//
// A hostname is NOT. QHostAddress only parses literals, so every route probe
// for a hostname comes back empty - which means "nothing to classify", never
// "no route exists". Reading that emptiness as a negative verdict would take
// every host addressed by name offline the moment a mask other than NLT_ALL is
// set, since netRoutesTo() can never succeed for a name.
//
// Callers MUST gate on this before treating an empty probe as an exclusion.
// The one address that can be a hostname rather than a literal is
// NvComputer::manualAddress, which addNewHostManually() fills from whatever the
// user typed, and ComputerManager resolves it into NvComputer::resolvedAddresses
// so there is normally a literal to filter. This predicate is what keeps a host
// reachable in the meantime, and what the filter falls back on when the
// resolution failed.
//
// The converse also holds and is load-bearing: once a literal exists, a hostname
// must not be treated as usable. It re-resolves at connect time, so reaching the
// host through it is reaching it over an unknown adapter, which is exactly what
// the mask was set up to prevent - and because NvComputer::addressesUnlocked()
// puts activeAddress first, a host once reached that way stays reached that way.
// NvComputer::allowedAddresses() enforces the pair; see there.
bool netAddressIsLiteral(const QString& address);

// Maps a NIC type onto the bit used by NvComputer::allowedLinkTypes
quint32 routeBitForInterface(QNetworkInterface::InterfaceType type);

// Whether `bit` is permitted by an NvComputer::allowedLinkTypes mask
bool isNetLinkTypeAllowed(NetLinkTypeBit bit, quint32 allowedBits);
