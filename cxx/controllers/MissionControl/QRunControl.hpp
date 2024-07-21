#include <array>
#include <memory>
#include <optional>
#include <QAbstractListModel>
#include <QSortFilterProxyModel>

#include "constellation/controller/Controller.hpp"

class QRunControl : public QAbstractListModel, public constellation::controller::Controller {

    Q_OBJECT

public:
    QRunControl(std::string_view controller_name, QObject* parent = 0);

    int rowCount(const QModelIndex& /*unused*/) const override { return connections_.size(); }

    int columnCount(const QModelIndex& /*unused*/) const override { return headers_.size(); }

    QVariant data(const QModelIndex& index, int role) const override;

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    std::optional<std::string> sendQCommand(const QModelIndex& index,
                                            const std::string& verb,
                                            const CommandPayload& payload = {});

    constellation::config::Dictionary getQCommands(const QModelIndex& index);

    std::string getQName(const QModelIndex& index) const;

signals:
    void connectionsChanged(std::size_t connections);
    void reachedGlobalState(constellation::protocol::CSCP::State state);

protected:
    void reached_state(constellation::protocol::CSCP::State state) override;
    void propagate_update(std::size_t position) override;
    void prepare_update(bool added, std::size_t position) override;
    void finalize_update(bool added, std::size_t connections) override;

private:
    static constexpr std::array<std::string, 6> headers_ {
        "Type", "Name", "State", "Connection", "Last response", "Last message"};

    std::size_t current_rows_ {0};
};

class QRunControlSortProxy : public QSortFilterProxyModel {

    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
};
