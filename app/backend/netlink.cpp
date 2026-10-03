#include "netlink.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QUdpSocket>

#include <algorithm>
#include <mutex>
#include <utility>

NetLinkTypeBit NetRoute::bit() const
{
    return static_cast<NetLinkTypeBit>(routeBitForInterface(interfaceType));
}

quint32 routeBitForInterface(QNetworkInterface::InterfaceType type)
{
    switch (type) {
    case QNetworkInterface::Ethernet:
        return NLT_WIRED;
    case QNetworkInterface::Wifi:
        return NLT_WIRELESS;
    case QNetworkInterface::Virtual:
    case QNetworkInterface::Ppp:
        return NLT_VIRTUAL;
    default:
        return NLT_OTHER;
    }
}

// Lower ranks sort first. This is what makes a wired route win over a wireless
// one when both are on-link for the same host.
static int routeRank(QNetworkInterface::InterfaceType type)
{
    switch (type) {
    case QNetworkInterface::Ethernet:
        return 0;
    case QNetworkInterface::Wifi:
        return 1;
    case QNetworkInterface::Virtual:
    case QNetworkInterface::Ppp:
        return 2;
    default:
        return 3;
    }
}

static NetRoute makeRoute(QNetworkInterface::InterfaceType type,
                          const QString& description,
                          const QHostAddress& localAddress,
                          int prefixLength,
                          const QHostAddress& remoteAddress)
{
    NetRoute route;
    route.interfaceType = type;
    route.nicDescription = description;
    route.localAddress = localAddress;
    route.prefixLength = prefixLength;

    // NB: isInSubnet() returns false when the protocol differs, so an IPv4
    // entry never matches an IPv6 remote address (and vice versa).
    route.onLink = prefixLength >= 0 && !localAddress.isNull() &&
                   localAddress.isInSubnet(remoteAddress, prefixLength);

    return route;
}

// Warns once per address that we could not work out a source address for.
//
// The once is about the log, not about the route. The same failure is hit
// again on every poll for every host that knows the address, because
// NvComputer::allowedAddresses() probes each one, and again from the model
// roles that answer the PC settings page - so a warning per occurrence would
// be the same line over and over, several times a second, with nothing in it
// the first one did not already say. Reachability itself is re-probed on the
// cache TTL; see the route cache comment below for why it must be.
//
// The routing table is global, so sharing this set across hosts is exactly the
// right scope: the verdict depends only on the address and the table, not on
// the PC we happened to be asking about, and no per-PC state (allowedLinkTypes
// in particular) takes part in the decision.
static bool noteUnroutableAddress(const QHostAddress& remoteAddress, const QString& reason)
{
    static QMutex mutex;
    static QSet<QHostAddress> reported;

    const QMutexLocker lock(&mutex);
    if (reported.contains(remoteAddress)) {
        return false;
    }

    reported.insert(remoteAddress);
    qWarning() << "Unable to determine the local source address for" << remoteAddress
               << ":" << reason;
    return true;
}

// Ask the kernel which source address (and therefore which NIC) it would
// actually use to reach `remoteAddress`.
static NetRoute findPreferredRoute(const QHostAddress& remoteAddress, uint16_t port)
{
    NetRoute none;

    if (remoteAddress.isNull()) {
        return none;
    }

    // A UDP 'connect' only binds the local end and asks the kernel to select a
    // route. No packet is ever sent, so this is instant, silent, and unaffected
    // by the local firewall. localAddress() then reports exactly which source
    // address the OS would use, which accounts for prefix length and interface
    // metric just like a real connection would.
    QUdpSocket probe;
    probe.connectToHost(remoteAddress, port);
    if (!probe.waitForConnected(1000)) {
        noteUnroutableAddress(remoteAddress, probe.errorString());
        return none;
    }

    const QHostAddress sourceAddress = probe.localAddress();
    if (sourceAddress.isNull()) {
        noteUnroutableAddress(remoteAddress, QStringLiteral("no source address available"));
        return none;
    }

    for (const QNetworkInterface& nic : QNetworkInterface::allInterfaces()) {
        if (!(nic.flags() & QNetworkInterface::IsUp) || (nic.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const QNetworkAddressEntry& entry : nic.addressEntries()) {
            if (entry.ip() != sourceAddress) {
                continue;
            }

            NetRoute route = makeRoute(nic.type(), nic.humanReadableName(), entry.ip(),
                                       entry.prefixLength(), remoteAddress);
            route.isPreferred = true;
            return route;
        }
    }

    return none;
}

// ---------------------------------------------------------------------------
// Route cache
//
// netRoutesTo() costs a UDP connect plus two QNetworkInterface::allInterfaces()
// enumerations, and the same handful of (address, port) pairs is asked about
// repeatedly: NvComputer::allowedAddresses() probes every address it knows for
// every host on every 3s poll round, getConflictingLinkDescription() asks about
// the active address right after, and the model roles plus
// Session::startConnectionAsync() each ask once more. The answer only changes
// when the routing table does, so it is memoized for a short window.
//
// The TTL matches the poll interval, so consecutive rounds share an entry and
// only the first address of each host misses.
//
// An empty result expires on the same TTL as any other. It is tempting to keep
// it forever on the argument that being unable to route to an address is a
// property of the address rather than a condition that resolves on its own, but
// that premise is false: reachability is a property of the current routing
// table, which changes under us for reasons that have nothing to do with the
// address - the cable is pulled, an interface is still coming up, Wi-Fi roams,
// DHCP renegotiates. Worse, the transient failures are the common ones. The
// source address a UDP connect reports stays valid while the link drops, so
// findPreferredRoute()'s interface lookup misses it and returns none even
// though the routing table is fine; the on-link scan below skips the same
// interface for the same reason, and the pair yields an empty result. Caching
// that forever makes the host unreachable until the process restarts, with
// nothing to retry it: allowedAddresses() drops the address, the poller stops
// probing, and the host can never learn the link came back.
//
// noteUnroutableAddress() keeps its own once-per-address set, which is a
// separate concern and is unaffected: the warning is noise, and noise is
// deduplicated forever, while the cached verdict is a decision that has to be
// re-made whenever the table it was read from moves.
//
// The cache is reachable from the poller, from mDNS add tasks, from the GUI
// thread and from the streaming path, so it needs a lock - but never one held
// across the probe, which would trade a per-computer stall for a global one.
// Two threads racing on the same key both compute and the last writer wins,
// which is harmless for a value derived from a table they both just read.
// ---------------------------------------------------------------------------

// A client knows a handful of hosts, so this only guards against unbounded
// growth as addresses churn (mDNS re-resolves, DHCP renews). Overflowing it
// drops the whole cache, which then refills over the next poll round.
constexpr int kRouteCacheMaxEntries = 256;

constexpr qint64 kRouteCacheTtlMs = 3000;

struct RouteCacheKey
{
    QHostAddress address;
    quint16 port = 0;

    // QHostAddress::operator== compares the protocol too, so an IPv4 and a
    // mapped IPv6 address for the same host stay distinct keys on their own.
    bool operator==(const RouteCacheKey& other) const
    {
        return port == other.port && address == other.address;
    }
};

size_t qHash(const RouteCacheKey& key, size_t seed = 0)
{
    return qHashMulti(seed, key.address, key.port);
}

struct RouteCacheEntry
{
    QVector<NetRoute> routes;
    qint64 expiresAtMs = 0;
};

QMutex g_RouteCacheMutex;
QHash<RouteCacheKey, RouteCacheEntry> g_RouteCache;

// One monotonic clock for every expiry, so an NTP step or a user changing the
// system time cannot make entries outlive their TTL or expire all at once.
qint64 routeCacheNowMs()
{
    static QElapsedTimer timer;
    static std::once_flag once;
    std::call_once(once, []() { timer.start(); });
    return timer.elapsed();
}

// Copies the cached routes into `routes` and returns true on a hit.
bool lookupCachedRoutes(const RouteCacheKey& key, QVector<NetRoute>& routes)
{
    const QMutexLocker lock(&g_RouteCacheMutex);

    const auto it = g_RouteCache.constFind(key);
    if (it == g_RouteCache.constEnd()) {
        return false;
    }

    if (it->expiresAtMs <= routeCacheNowMs()) {
        g_RouteCache.erase(it);
        return false;
    }

    routes = it->routes;
    return true;
}

void storeCachedRoutes(const RouteCacheKey& key, const QVector<NetRoute>& routes)
{
    const QMutexLocker lock(&g_RouteCacheMutex);

    if (g_RouteCache.size() >= kRouteCacheMaxEntries && !g_RouteCache.contains(key)) {
        g_RouteCache.clear();
    }

    RouteCacheEntry entry;
    entry.routes = routes;
    entry.expiresAtMs = routeCacheNowMs() + kRouteCacheTtlMs;

    g_RouteCache.insert(key, entry);
}

QVector<NetRoute> netRoutesTo(const QHostAddress& remoteAddress, uint16_t port)
{
    QVector<NetRoute> routes;

    if (remoteAddress.isNull()) {
        return routes;
    }

    const RouteCacheKey cacheKey { remoteAddress, port };
    if (lookupCachedRoutes(cacheKey, routes)) {
        return routes;
    }

    // 1. The route the kernel itself would pick. This must be collected even
    //    when the host is not on-link, because that is the gateway path used
    //    for hosts living on a different subnet.
    const NetRoute preferred = findPreferredRoute(remoteAddress, port);
    if (preferred.isUsable()) {
        routes.append(preferred);
    }

    // 2. Every other NIC that also has the host inside its subnet. These are
    //    the alternatives the user may want to choose between, and they are
    //    the reason we cannot simply trust the first address mDNS hands us.
    for (const QNetworkInterface& nic : QNetworkInterface::allInterfaces()) {
        if (!(nic.flags() & QNetworkInterface::IsUp) || (nic.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const QNetworkAddressEntry& entry : nic.addressEntries()) {
            const NetRoute route = makeRoute(nic.type(), nic.humanReadableName(), entry.ip(),
                                             entry.prefixLength(), remoteAddress);
            if (!route.onLink) {
                continue;
            }

            // The preferred route was already appended above
            bool duplicate = false;
            for (const NetRoute& existing : std::as_const(routes)) {
                if (existing.localAddress == route.localAddress) {
                    duplicate = true;
                    break;
                }
            }

            if (!duplicate) {
                routes.append(route);
            }
        }
    }

    std::stable_sort(routes.begin(), routes.end(),
                     [](const NetRoute& lhs, const NetRoute& rhs) {
        if (lhs.onLink != rhs.onLink) {
            return lhs.onLink;
        }

        const int lhsRank = routeRank(lhs.interfaceType);
        const int rhsRank = routeRank(rhs.interfaceType);
        if (lhsRank != rhsRank) {
            return lhsRank < rhsRank;
        }

        // The kernel's own choice wins among otherwise equal routes
        return lhs.isPreferred && !rhs.isPreferred;
    });

    storeCachedRoutes(cacheKey, routes);
    return routes;
}

NetRoute bestAllowedRoute(const QVector<NetRoute>& routes, quint32 allowedBits)
{
    for (const NetRoute& route : routes) {
        if (route.isUsable() && (allowedBits & routeBitForInterface(route.interfaceType))) {
            return route;
        }
    }

    return NetRoute();
}

NetLinkTypeBit netLinkTypeOf(const QHostAddress& remoteAddress, uint16_t port)
{
    const QVector<NetRoute> routes = netRoutesTo(remoteAddress, port);

    // netRoutesTo() sorts the kernel's own choice first for on-link routes, so
    // this reflects what the OS will actually do.
    for (const NetRoute& route : routes) {
        if (route.isPreferred && route.isUsable()) {
            return route.bit();
        }
    }

    for (const NetRoute& route : routes) {
        if (route.isUsable()) {
            return route.bit();
        }
    }

    return NLT_OTHER;
}

bool isNetLinkTypeAllowed(NetLinkTypeBit bit, quint32 allowedBits)
{
    return (allowedBits & static_cast<quint32>(bit)) != 0;
}

bool netAddressIsLiteral(const QString& address)
{
    // setAddress() only parses literals and leaves the QHostAddress null for
    // anything else, so its return value is exactly the question being asked.
    // The same idiom is used in NvComputer::wake() to skip a reverse lookup.
    QHostAddress literal;
    return literal.setAddress(address);
}
