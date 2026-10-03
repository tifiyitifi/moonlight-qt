#include "backend/computermanager.h"
#include "streaming/session.h"

#include <QAbstractListModel>
#include <QObject>

class ComputerModel : public QAbstractListModel
{
    Q_OBJECT

    enum Roles
    {
        NameRole = Qt::UserRole,
        OnlineRole,
        PairedRole,
        BusyRole,
        WakeableRole,
        StatusUnknownRole,
        ServerSupportedRole,
        DetailsRole,

        // New entries must go at the end so existing role values are unchanged
        AllowedLinkTypesRole,
        ActiveRouteDescriptionRole,
        ConnectionConflictRole,
        UnenforceableLinkFilterRole,
        MaskExcludedLinkFilterRole,
        NameSuppressedLinkFilterRole
    };

    enum NetLinkTypes
    {
        NLT_MASK_WIRED    = 1,
        NLT_MASK_WIRELESS = 2,
        NLT_MASK_VIRTUAL  = 4,
        NLT_MASK_OTHER    = 8,
        NLT_MASK_ALL      = 15
    };
    Q_ENUM(NetLinkTypes)

public:
    explicit ComputerModel(QObject* object = nullptr);

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager);

    // Exposes the roles of one specific computer as plain properties, so a page
    // can present a single host without needing a delegate. Future per-PC
    // settings pages can read and write through this same interface.
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentChanged)
    Q_PROPERTY(int currentAllowedLinkTypes READ currentAllowedLinkTypes NOTIFY currentChanged)
    Q_PROPERTY(QString currentActiveRoute READ currentActiveRoute NOTIFY currentChanged)
    Q_PROPERTY(QString currentConnectionConflict READ currentConnectionConflict NOTIFY currentChanged)
    // Set when the mask cannot be enforced for this PC at all - see
    // NvComputer::getUnclassifiableAddress().
    Q_PROPERTY(QString currentUnenforceableLinkFilter READ currentUnenforceableLinkFilter NOTIFY currentChanged)
    // Set when every address Moonlight holds for this PC is excluded by the
    // mask, which is why the PC is offline - see
    // NvComputer::getMaskExcludedLinkTypes(). A different fact from the one
    // above: there the mask is not in effect on a PC that is reachable, here it
    // is in effect on a PC that is not.
    Q_PROPERTY(QString currentMaskExcludedLinkFilter READ currentMaskExcludedLinkFilter NOTIFY currentChanged)
    // Set when the mask approved an address for this PC, that address is not
    // answering, and the name which would still work is deliberately not used
    // because a mask cannot be applied to it - see
    // NvComputer::getNameSuppressedAddress(). The third distinct fact, and the
    // only one whose remedy is not in the connection type selection: nothing
    // there is wrong, the address is simply out of date.
    Q_PROPERTY(QString currentNameSuppressedLinkFilter READ currentNameSuppressedLinkFilter NOTIFY currentChanged)
    // Compile-time state of the per-PC connection type filter, so QML can hide
    // the UI that drives it. CONSTANT because NetLinkFilter::kEnabled is a
    // constexpr, so the value can never change within a running process.
    Q_PROPERTY(bool netLinkFilterEnabled READ netLinkFilterEnabled CONSTANT)

    int currentIndex() const;
    void setCurrentIndex(int index);

    // Resolve the selection by host identity. Returns false when no host
    // carries this uuid (e.g. the host was deleted while the page was open).
    Q_INVOKABLE bool setCurrentIndexByUuid(const QString& uuid);

    Q_INVOKABLE QString getComputerUuid(int computerIndex) const;

    QString currentName() const;
    int currentAllowedLinkTypes() const;
    QString currentActiveRoute() const;
    QString currentConnectionConflict() const;
    QString currentUnenforceableLinkFilter() const;

    QString currentMaskExcludedLinkFilter() const;

    QString currentNameSuppressedLinkFilter() const;

    bool netLinkFilterEnabled() const;

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void deleteComputer(int computerIndex);

    Q_INVOKABLE QString generatePinString();

    Q_INVOKABLE void pairComputer(int computerIndex, QString pin);

    Q_INVOKABLE void testConnectionForComputer(int computerIndex);

    Q_INVOKABLE void wakeComputer(int computerIndex);

    Q_INVOKABLE void renameComputer(int computerIndex, QString name);

    Q_INVOKABLE void setConnectionOptionsForComputer(int computerIndex, int allowedLinkTypes);

    // Applies the mask to the host selected via currentIndex. Unlike
    // setConnectionOptionsForComputer() this cannot target the wrong PC after
    // the host list has been reordered, because currentIndex is re-resolved by
    // uuid whenever the list changes.
    Q_INVOKABLE void setConnectionOptionsForCurrentComputer(int allowedLinkTypes);

    // Human readable name for a NetLinkTypes mask value, used by the details
    // dialog and the per-PC settings page.
    Q_INVOKABLE QString netLinkTypeName(int netLinkType) const;

    Q_INVOKABLE Session* createSessionForCurrentGame(int computerIndex);

signals:
    void pairingCompleted(QVariant error);
    void connectionTestCompleted(int result, QString blockedPorts);
    void currentChanged();

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handlePairingCompleted(NvComputer* computer, QString error);

private:
    QString describeAllowedLinkTypes(quint32 allowedLinkTypes) const;

    // "Ethernet (Wired), Wi-Fi (Wireless)" for the bits in `linkTypes`, in the
    // fixed order the checkboxes use, so the same type reads the same way
    // whichever side of the conversation names it. Shared by the description of
    // the current selection and by the one naming the types a host is being
    // excluded on.
    QString linkTypeMaskDescription(quint32 linkTypes) const;

    QVariant currentValue(int role) const;

    // Re-points m_CurrentIndex at the host named by m_CurrentUuid, or at -1
    // when that host is gone. Must be called after any change to m_Computers,
    // since a positional index silently starts naming a different PC.
    void reindexCurrentComputer();

    QVector<NvComputer*> m_Computers;
    ComputerManager* m_ComputerManager;
    int m_CurrentIndex;

    // UUID of the host currentIndex points at. Indices are positional, so the
    // selection has to be tracked by identity to survive a list refresh.
    QString m_CurrentUuid;
};
