#pragma once

#include <QWidget>

class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QResizeEvent;
class TransferTask;

struct TransferCardData {
    bool upload = true;
    QString fileName;
    QString peerName;
    QString localPath;
    QString stateText;
    QString stateKey;
    QString error;
    qint64 size = 0;
    qint64 transferred = 0;
};

// A compact, self-contained transfer row. QTreeWidget still owns selection and
// the task model; this widget is only responsible for presentation and actions.
class TransferTaskCard final : public QWidget {
    Q_OBJECT
public:
    explicit TransferTaskCard(QWidget *parent = nullptr);

    void setTask(const TransferTask &task, const QString &peerName);
    void setData(const TransferCardData &data);
    void setSelected(bool selected);

signals:
    void clicked();
    void openDirectoryRequested(const QString &localPath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateElidedText();

    TransferCardData m_data;
    QFrame *m_surface = nullptr;
    QLabel *m_direction = nullptr;
    QLabel *m_fileName = nullptr;
    QLabel *m_peer = nullptr;
    QLabel *m_percent = nullptr;
    QLabel *m_bytes = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_path = nullptr;
    QLabel *m_error = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_openDirectory = nullptr;
};
