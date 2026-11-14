#include "TransferTaskCard.h"

#include "TransferManager.h"

#include <QDir>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QVBoxLayout>

namespace {
QString formattedBytes(qint64 bytes)
{
    bytes = qMax<qint64>(0, bytes);
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GiB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

class TransferLinkLabel final : public QLabel {
    Q_OBJECT
public:
    explicit TransferLinkLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setMinimumWidth(0);
    }

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
            emit clicked();
            event->accept();
            return;
        }
        QLabel::mouseReleaseEvent(event);
    }
};
} // namespace

TransferTaskCard::TransferTaskCard(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("transferTaskCardHost"));
    setProperty("selected", false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 6);
    outerLayout->setSpacing(0);
    m_surface = new QFrame(this);
    m_surface->setObjectName(QStringLiteral("transferTaskCard"));
    m_surface->setProperty("selected", false);
    outerLayout->addWidget(m_surface);

    auto *layout = new QVBoxLayout(m_surface);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(6);

    auto *top = new QHBoxLayout;
    top->setSpacing(10);
    m_direction = new QLabel(m_surface);
    m_direction->setObjectName(QStringLiteral("transferDirection"));
    top->addWidget(m_direction, 0, Qt::AlignVCenter);

    auto *fileName = new TransferLinkLabel(m_surface);
    m_fileName = fileName;
    m_fileName->setObjectName(QStringLiteral("transferFileName"));
    m_fileName->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    top->addWidget(m_fileName, 1, Qt::AlignVCenter);

    m_state = new QLabel(m_surface);
    m_state->setObjectName(QStringLiteral("transferState"));
    m_state->setAlignment(Qt::AlignCenter);
    top->addWidget(m_state, 0, Qt::AlignVCenter);

    m_openDirectory = new QPushButton(QStringLiteral("打开目录"), m_surface);
    m_openDirectory->setObjectName(QStringLiteral("openTransferCardDirectoryButton"));
    m_openDirectory->setToolTip(QStringLiteral("打开此文件所在的本地目录"));
    top->addWidget(m_openDirectory, 0, Qt::AlignVCenter);
    layout->addLayout(top);

    auto *metadata = new QHBoxLayout;
    metadata->setSpacing(14);
    m_peer = new QLabel(m_surface);
    m_peer->setObjectName(QStringLiteral("transferPeer"));
    m_peer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    metadata->addWidget(m_peer, 1);
    m_percent = new QLabel(m_surface);
    m_percent->setObjectName(QStringLiteral("transferPercent"));
    metadata->addWidget(m_percent);
    m_bytes = new QLabel(m_surface);
    m_bytes->setObjectName(QStringLiteral("transferBytes"));
    metadata->addWidget(m_bytes);
    layout->addLayout(metadata);

    m_progress = new QProgressBar(m_surface);
    m_progress->setObjectName(QStringLiteral("transferProgress"));
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(8);
    layout->addWidget(m_progress);

    auto *path = new TransferLinkLabel(m_surface);
    m_path = path;
    m_path->setObjectName(QStringLiteral("transferPath"));
    m_path->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(m_path);

    m_error = new QLabel(m_surface);
    m_error->setObjectName(QStringLiteral("transferError"));
    m_error->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(m_error);
    for (QLabel *label : {m_fileName, m_path, m_error})
        label->installEventFilter(this);

    for (QLabel *label : {m_direction, m_peer, m_percent, m_bytes, m_state, m_error})
        label->setAttribute(Qt::WA_TransparentForMouseEvents);

    connect(fileName, &TransferLinkLabel::clicked, this, [this] {
        emit openDirectoryRequested(m_data.localPath);
    });
    connect(path, &TransferLinkLabel::clicked, this, [this] {
        emit openDirectoryRequested(m_data.localPath);
    });
    connect(m_openDirectory, &QPushButton::clicked, this, [this] {
        emit openDirectoryRequested(m_data.localPath);
    });
}

void TransferTaskCard::setTask(const TransferTask &task, const QString &peerName)
{
    TransferCardData data;
    data.upload = task.direction() == TransferTask::Direction::Upload;
    data.fileName = task.fileName();
    data.peerName = peerName;
    data.localPath = QDir::cleanPath(task.localPath());
    data.stateText = task.stateText();
    data.error = task.error();
    data.size = task.size();
    data.transferred = task.transferredBytes();
    switch (task.state()) {
    case TransferTask::State::Paused: data.stateKey = QStringLiteral("paused"); break;
    case TransferTask::State::Completed: data.stateKey = QStringLiteral("completed"); break;
    case TransferTask::State::Failed: data.stateKey = QStringLiteral("failed"); break;
    case TransferTask::State::Cancelled: data.stateKey = QStringLiteral("cancelled"); break;
    case TransferTask::State::Queued:
    case TransferTask::State::Preparing: data.stateKey = QStringLiteral("queued"); break;
    case TransferTask::State::Uploading:
    case TransferTask::State::Downloading: data.stateKey = QStringLiteral("active"); break;
    }
    setData(data);
}

void TransferTaskCard::setData(const TransferCardData &data)
{
    const bool directionChanged = m_data.upload != data.upload;
    const bool stateChanged = m_data.stateKey != data.stateKey;
    m_data = data;
    setProperty("stateKey", data.stateKey);
    m_direction->setText(data.upload ? QStringLiteral("↑  上传") : QStringLiteral("↓  下载"));
    m_direction->setProperty("direction", data.upload ? QStringLiteral("upload")
                                                      : QStringLiteral("download"));
    m_direction->setToolTip(data.upload ? QStringLiteral("发送到对方") : QStringLiteral("从对方接收"));
    m_peer->setText(QStringLiteral("对方：%1").arg(data.peerName));
    m_peer->setToolTip(data.peerName);
    m_state->setText(data.stateText);
    m_state->setProperty("state", data.stateKey);
    m_state->setToolTip(data.stateText);

    const int percent = data.size > 0
        ? qBound(0, static_cast<int>(data.transferred * 100 / data.size), 100)
        : data.stateKey == QLatin1String("completed") ? 100 : 0;
    m_percent->setText(QStringLiteral("%1%").arg(percent));
    m_bytes->setText(QStringLiteral("%1 / %2").arg(formattedBytes(data.transferred),
                                                   formattedBytes(data.size)));
    m_progress->setValue(percent);
    m_progress->setToolTip(QStringLiteral("%1% · %2 / %3")
        .arg(percent).arg(formattedBytes(data.transferred), formattedBytes(data.size)));
    m_progress->setProperty("state", data.stateKey);

    const QString nativePath = QDir::toNativeSeparators(data.localPath);
    m_fileName->setToolTip(QStringLiteral("%1\n单击打开所在目录").arg(data.fileName));
    m_path->setToolTip(QStringLiteral("%1\n单击打开所在目录").arg(nativePath));
    m_openDirectory->setEnabled(!data.localPath.trimmed().isEmpty());
    m_error->setVisible(!data.error.isEmpty());
    m_error->setToolTip(data.error);
    updateElidedText();

    // Progress can change many times per second; only dynamic state properties
    // need repolishing, not every byte update.
    if (directionChanged) {
        m_direction->style()->unpolish(m_direction);
        m_direction->style()->polish(m_direction);
    }
    if (stateChanged) {
        m_state->style()->unpolish(m_state);
        m_state->style()->polish(m_state);
        m_progress->style()->unpolish(m_progress);
        m_progress->style()->polish(m_progress);
    }
    updateGeometry();
}

void TransferTaskCard::setSelected(bool selected)
{
    if (property("selected").toBool() == selected)
        return;
    setProperty("selected", selected);
    m_surface->setProperty("selected", selected);
    m_surface->style()->unpolish(m_surface);
    m_surface->style()->polish(m_surface);
    m_surface->update();
}

void TransferTaskCard::updateElidedText()
{
    if (!m_fileName || !m_path)
        return;
    const int fileWidth = qMax(40, m_fileName->width());
    m_fileName->setText(m_fileName->fontMetrics().elidedText(
        m_data.fileName, Qt::ElideMiddle, fileWidth));
    const int pathWidth = qMax(40, m_path->width());
    const QString nativePath = QDir::toNativeSeparators(m_data.localPath);
    m_path->setText(m_path->fontMetrics().elidedText(
        QStringLiteral("本地：%1").arg(nativePath), Qt::ElideMiddle, pathWidth));
    const QString error = QStringLiteral("错误：%1").arg(m_data.error);
    m_error->setText(m_error->fontMetrics().elidedText(
        error, Qt::ElideRight, qMax(40, m_error->width())));
}

bool TransferTaskCard::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize
        && (watched == m_fileName || watched == m_path || watched == m_error))
        updateElidedText();
    return QWidget::eventFilter(watched, event);
}

void TransferTaskCard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit clicked();
    QWidget::mousePressEvent(event);
}

void TransferTaskCard::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateElidedText();
}

#include "TransferTaskCard.moc"
