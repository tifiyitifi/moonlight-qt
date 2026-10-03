#pragma once

#include "nvhttp.h"
#include "nvaddress.h"

#include <QThread>
#include <QReadWriteLock>
#include <QSettings>
#include <QRunnable>

class CopySafeReadWriteLock : public QReadWriteLock
{
public:
    CopySafeReadWriteLock() = default;

    // Don't actually copy the QReadWriteLock
    CopySafeReadWriteLock(const CopySafeReadWriteLock&) : QReadWriteLock() {}
    CopySafeReadWriteLock& operator=(const CopySafeReadWriteLock &) { return *this; }
};

class NvComputer
{
    friend class PcMonitorThread;
    friend class ComputerManager;
    friend class PendingQuitTask;

private:
    void sortAppList();

    // The body of uniqueAddresses() without taking the lock. Callers that
    // already hold the computer lock for write (update()) must use this,
    // since CopySafeReadWriteLock is not recursive.
    QVector<NvAddress>
    addressesUnlocked() const;

    // What one pass over the address list has to say about the mask: whether we
    // hold a literal it approves, whether we hold one at all, which connection
    // types the literals we do hold are reachable over, and whether the poller
    // currently has the host online.
    //
    // The fourth field is the set of types with a USABLE route to some literal,
    // masked against nothing - it is a property of the routing table, not of
    // allowedLinkTypes, and is only meaningful when anyLiteralAllowed is false,
    // where it names the types the user has deselected and the host is
    // therefore excluded on. It is 0 when no literal is reachable at all, which
    // is "no path we could work out" rather than "a path you excluded", and the
    // two must not be reported with the same words.
    //
    // hostOnline is the one field that is not about the address list, and it is
    // snapshotted inside the same locked scope as everything else because the
    // poller writes state without any lock at all: reading it in a second
    // critical section would make the two halves describe different moments.
    struct LinkFilterProbe
    {
        QString name;
        bool sawLiteral = false;
        bool anyLiteralAllowed = false;
        bool hostOnline = false;
        quint32 reachableTypes = 0;
    };

    // The single pass shared by getUnclassifiableAddress(),
    // getMaskExcludedLinkTypes() and getNameSuppressedAddress(). The three
    // questions they answer are decided by the same evidence and split into
    // three different failures by the user, so they must not be answered by
    // passes that can disagree. netRoutesTo() memoizes per (address, port) for a
    // poll interval anyway, so the later public accessors are cache hits rather
    // than extra rounds of probes.
    LinkFilterProbe
    probeLinkFilter() const;

    bool updateAppList(QVector<NvApp> newAppList);

    bool pendingQuit;

public:
    NvComputer() = default;

    // Caller is responsible for synchronizing read access to the other host
    NvComputer(const NvComputer&) = default;

    // Caller is responsible for synchronizing read access to the other host
    NvComputer& operator=(const NvComputer &) = default;

    explicit NvComputer(NvHTTP& http, QString serverInfo);

    explicit NvComputer(QSettings& settings);

    void
    setRemoteAddress(QHostAddress);

    bool
    update(const NvComputer& that);

    bool
    wake() const;

    enum ReachabilityType
    {
        RI_UNKNOWN,
        RI_LAN,
        RI_VPN,
    };

    ReachabilityType
    getActiveAddressReachability() const;

    QVector<NvAddress>
    uniqueAddresses() const;

    // uniqueAddresses() filtered by the per-PC connection type options.
    //
    // The mask gates ADDRESSES, not adapters: an address with no route over a
    // connection type the user allows is dropped, and when that leaves nothing
    // the list is empty and the host reports as offline, rather than falling
    // back to an excluded adapter.
    //
    // The order is uniqueAddresses()'s and nothing re-sorts it: the address
    // that is already working stays first, the rest follow the order mDNS
    // reported them in, and the WAN/IPv6/manual addresses come last.
    // netRoutesTo()'s route ranking is consulted only to decide whether an
    // address is kept at all, never to order the survivors. So the mask reads
    // as "wired wins over wireless" only when the two connection types sit on
    // different subnets, where the wireless address has no wired route at all
    // and is dropped. When the host has two NICs in one subnet, both addresses
    // have a route over every one of our adapters that shares it, both survive,
    // and whichever mDNS reported first is the one that keeps being probed.
    //
    // Keeping the working address first is deliberate, not incidental: sorting
    // the survivors by rank would re-probe a ranked-first but unreachable
    // address ahead of the latched one on every polling round, costing a
    // FAST_FAIL_TIMEOUT_MS each time.
    //
    // A hostname (a non-literal) is a special case, because netRoutesTo() cannot
    // probe one at all: the empty result means "nothing to classify", never "no
    // route". So it is kept ONLY while the host holds no literal at all - before
    // resolution lands, or when resolution failed - and dropped as soon as one
    // does. By then the name is standing in for a real address, and keeping it
    // would let it resurrect a route the mask rejected: it re-resolves at connect
    // time, so a name answering only on the excluded WiFi address would bring a
    // wired-only host back online over exactly the connection type that was
    // excluded. It would also stick, since addressesUnlocked() puts activeAddress
    // first, so the name would keep outranking the literal it was standing in
    // for and the RTSP pin in Session::startConnectionAsync() would never narrow
    // anything.
    //
    // The pair therefore holds: activeAddress is a name if and only if we hold no
    // literal to judge. That is what makes the three states below exhaustive,
    // and ComputerManager::resolveHostAddresses() is what turns a name into
    // literals in the first place.
    //
    // Every state is reported, by a different function because they are different
    // failures. While no literal has landed the name is the only address there is
    // and the mask is simply not in effect (getUnclassifiableAddress()). Once one
    // has landed the mask is in effect: either it rejects every one of them and
    // the host is offline (getMaskExcludedLinkTypes()), or it approves one, the
    // name is dropped, and the host is offline anyway because the approved
    // address is not answering - a fact the mask cannot fix and the name cannot
    // be used to work around, which is what getNameSuppressedAddress() reports.
    // None of the three is silent.
    //
    // It cannot gate the adapter the bytes actually leave through. When two
    // adapters share a subnet the destination address cannot select between
    // them and the OS interface metric decides, so an allowed route existing
    // does not mean the traffic uses it. That case is reported by
    // getConflictingLinkDescription(), which is the only backstop for it.
    QVector<NvAddress>
    allowedAddresses() const;

    // Describes the NIC the OS would actually use to reach activeAddress when
    // that NIC's connection type is not in allowedLinkTypes, or an empty string
    // when there is no conflict. The text is deliberately untranslated so the
    // caller can wrap it in its own tr() context.
    QString
    getConflictingLinkDescription() const;

    // The address string the connection type filter has nothing to say about,
    // or an empty string when there is none.
    //
    // Non-empty means all three of these hold: the mask is not NLT_ALL, the
    // host carries a non-literal (a hostname, which netRoutesTo() cannot probe),
    // and we hold no literal to judge it against - the name has not resolved to
    // anything, or resolution has not landed yet. allowedAddresses() keeps the
    // name in that state, so nothing is excluded and the poller still probes it;
    // when the host does answer, the stream goes out over whatever the name
    // resolves to at connect time. The user has asked for a guarantee this PC
    // cannot give, and the address returned is the name that is standing in for
    // a real one. So a non-empty return here means the name is IN USE right now,
    // which is the whole difference from the two accessors below.
    //
    // This is a different failure from getMaskExcludedLinkTypes(), which
    // describes the state where a literal DOES exist and the mask rejected
    // every one of them, and from getNameSuppressedAddress(), which describes
    // the state where a literal exists, the mask approved one, and it is not
    // answering. In both of those the name is not in the poller's candidate list
    // and the host is offline, so nothing is streaming: describing that as a
    // stream about to go out would be the opposite of the truth, and this
    // function returns an empty string rather than do it.
    //
    // It is also a different failure from getConflictingLinkDescription():
    // there the mask is being honored but the OS overrides it by interface
    // metric, here there is no address to honor it with. All four are worth
    // telling the user about, separately.
    QString
    getUnclassifiableAddress() const;

    // The connection types this host is reachable over when every address we
    // hold for it is excluded by the mask, or 0 when that is not the case.
    //
    // Non-zero means all of these hold: the mask is not NLT_ALL, the host
    // carries at least one literal, and not one of them has a route over a
    // connection type the mask allows. That is the state in which
    // allowedAddresses() drops the hostname as well, leaving the poller an
    // empty candidate list and taking the host CS_OFFLINE - so the mask IS in
    // effect, the opposite of getUnclassifiableAddress() above, and the user
    // needs to hear that rather than be told a stream is about to go out.
    //
    // The value is a mask of the NetLinkTypeBit bits, which is what lets the
    // caller name the types in the user's own terms and point at the remedy:
    // selecting one of them brings the host back. 0 also covers the case where
    // no literal has a usable route at all, since "we could not work out a path"
    // is not the same failure as "the only path we found is one you excluded",
    // and the ordinary offline message is the honest thing to show for it.
    quint32
    getMaskExcludedLinkTypes() const;

    // The name we are refusing to fall back on, or an empty string when there is
    // no such name.
    //
    // Non-empty means all of these hold: the mask is not NLT_ALL, the host
    // carries a name, we hold at least one literal, the mask approves at least
    // one of them, and the host is not online. That is the state allowedAddresses()
    // leaves behind by dropping the name as soon as a literal exists: the poller
    // has approved literals to probe, none of them answered, and the one address
    // that would still have worked is the name - which re-resolves at connect
    // time and would go wherever DNS points, so using it here is precisely what
    // the mask was set up to prevent.
    //
    // This is the cost of enforcing the mask, and it is the one state the mask
    // cannot repair on its own: the approved address is usually simply not
    // answering (the PC is off, asleep, or has moved to a new address), and only
    // re-resolving the name can tell those apart. ComputerManager does that from
    // the polling loop when a named host fails, so the address here is usually a
    // stale one rather than a permanently broken name - which is what the caller
    // should tell the user to look at.
    QString
    getNameSuppressedAddress() const;

    // Whether any address this host carries is a name rather than a literal, and
    // so whether there is anything the connection type filter could gain from
    // resolving one. Cheap: it takes the computer lock and does no route probing.
    bool
    hasNameAddress() const;

    void
    serialize(QSettings& settings, bool serializeApps) const;

    // Caller is responsible for synchronizing read access to both hosts
    bool
    isEqualSerialized(const NvComputer& that) const;

    enum PairState
    {
        PS_UNKNOWN,
        PS_PAIRED,
        PS_NOT_PAIRED
    };

    enum ComputerState
    {
        CS_UNKNOWN,
        CS_ONLINE,
        CS_OFFLINE
    };

    // Ephemeral traits
    ComputerState state;
    PairState pairState;
    NvAddress activeAddress;
    uint16_t activeHttpsPort;
    int currentGameId;
    QString gfeVersion;
    QString appVersion;
    QVector<NvDisplayMode> displayModes;
    int maxLumaPixelsHEVC;
    int serverCodecModeSupport;
    QString gpuModel;
    bool isSupportedServerVersion;

    // Persisted traits
    NvAddress localAddress;
    NvAddress remoteAddress;
    NvAddress ipv6Address;
    NvAddress manualAddress;
    // Every address the host advertised over mDNS. Moonlight used to keep only
    // the first one it received, which made it impossible to fall back to a
    // different NIC for a multi-homed host. Only used when
    // NetLinkFilter::kEnabled is true.
    QVector<NvAddress> mdnsAddresses;
    // Literals obtained by resolving a non-literal manualAddress.
    //
    // The connection type filter classifies addresses, and it can only
    // classify literals: netRoutesTo() on a hostname probes nothing and comes
    // back empty. A host the user addressed by name would therefore have
    // nothing to filter and, under any mask other than NLT_ALL, would be taken
    // permanently offline. Resolving the name turns it back into something the
    // filter can judge, and putting these ahead of manualAddress in
    // addressesUnlocked() makes activeAddress settle on a literal - which is
    // what lets Session::startConnectionAsync() pin the stream to the address
    // the mask approved.
    //
    // Populated by ComputerManager only when a mask other than NLT_ALL is
    // requested, and cleared again when the mask goes back to NLT_ALL, so a
    // host with no user preference keeps the exact address list it had before
    // this feature existed.
    //
    // NB: the mask takes effect before the lookup does, so there is a window of
    // a DNS round trip plus up to one poll interval - a few seconds - in which
    // the host is polled over the unfiltered hostname. A stream started inside
    // that window gets the name, which the RTSP pin cannot narrow. Closing it
    // would mean tracking "resolution in flight" and holding the host offline
    // until it lands, which trades a few seconds of blip for a host that stays
    // unreachable for good whenever the lookup fails. Erring toward reachable
    // is the right side of that trade, so the window is documented rather than
    // closed.
    //
    // These also go stale, which nothing else can repair. A PC that keeps its
    // name and gets a different address - a new DHCP lease, another subnet,
    // another NIC - is ordinary, and once it happens the only address we would
    // probe is one it no longer has. ComputerManager therefore re-resolves a
    // named host from the polling loop when it stops answering, replacing this
    // list rather than appending to it; see
    // ComputerManager::handleReResolveHostAddresses(). Until that lands, and
    // whenever the name resolves to the same addresses, the mask is in full
    // effect and the host is simply offline - which
    // NvComputer::getNameSuppressedAddress() reports.
    QVector<NvAddress> resolvedAddresses;
    // Bitmask of NetLinkTypeBit values the user allows us to use to reach this
    // host. NB: This is user-owned state, so it must never be assigned by
    // update() — a polled NvComputer carries the NLT_ALL default.
    quint32 allowedLinkTypes;
    QByteArray macAddress;
    QString name;
    bool hasCustomName;
    QString uuid;
    QSslCertificate serverCert;
    QVector<NvApp> appList;
    bool isNvidiaServerSoftware;
    // Remember to update isEqualSerialized() when adding fields here!

    // Synchronization
    mutable CopySafeReadWriteLock lock;

    // Transient, non-persisted state: set the first time we log that the
    // connection type filter excluded every known address for this host, so the
    // offline warning is not repeated every poll. Reset when the mask changes.
    // It is deliberately outside the persisted block above so
    // isEqualSerialized() ignores it.
    mutable bool linkFilterFallbackLogged = false;

private:
    uint16_t externalPort;
};
