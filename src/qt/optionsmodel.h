#ifndef OPTIONSMODEL_H
#define OPTIONSMODEL_H

#include "netbase.h"

#include <QAbstractListModel>

extern bool fUseDarkTheme;


static constexpr unsigned short DEFAULT_GUI_PROXY_PORT = 9050;

/** Interface from Qt to configuration data structure for Bitcoin client.
   To Qt, the options are presented as a list with the different options
   laid out vertically.
   This can be changed to a tree once the settings become sufficiently
   complex.
 */
class OptionsModel : public QAbstractListModel
{
    Q_OBJECT

public:
    explicit OptionsModel(QObject *parent = 0);

    enum OptionID {
        StartAtStartup,         // bool
        MinimizeToTray,         // bool
        MapPortUPnP,            // bool
        MinimizeOnClose,        // bool
        ProxyUse,               // bool
        ProxyIP,                // QString
        ProxyPort,              // int
        ProxySocksVersion,      // DEPRECATED - int
        Fee,                    // qint64
        ReserveBalance,         // qint64
        DisplayUnit,            // BitcoinUnits::Unit
        DisplayAddresses,       // bool
        DetachDatabases,        // bool
        Language,               // QString
        CoinControlFeatures,    // bool
        DarksendRounds,         // int
        anonymizeNeutronAmount, //int
        UseDarkTheme,           // bool
        OptionIDRowCount,
    };

    void Init();
    void Reset();

    int rowCount(const QModelIndex & parent = QModelIndex()) const;
    QVariant data(const QModelIndex & index, int role = Qt::DisplayRole) const;
    bool setData(const QModelIndex & index, const QVariant & value, int role = Qt::EditRole);

    /* Explicit getters */
    qint64 getTransactionFee();
    qint64 getReserveBalance();
    bool getMinimizeToTray();
    bool getMinimizeOnClose();
    int getDisplayUnit();
    bool getDisplayAddresses();
    bool getCoinControlFeatures();

    QString getLanguage() { return language; }

    /* Restart flag helper */
    void setRestartRequired(bool fRequired);
    bool isRestartRequired() const;

private:
    int nDisplayUnit;
    bool bDisplayAddresses;
    bool fMinimizeToTray;
    bool fMinimizeOnClose;
    bool fCoinControlFeatures;
    QString language;
    /* settings that were overriden by command-line */
    QString strOverriddenByCommandLine;

    /// Add option to list of GUI options overridden through command line/config file
    void addOverriddenOption(const std::string& option);

signals:
    void displayUnitChanged(int unit);
    void transactionFeeChanged(qint64);
    void reserveBalanceChanged(qint64);
    void coinControlFeaturesChanged(bool);
    void darksendRoundsChanged(int);
    void anonymizeNeutronAmountChanged(int);
};

#endif // OPTIONSMODEL_H
