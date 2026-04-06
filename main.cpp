#include <QtWidgets>
#include <QtCore>
#include <QtGui>

struct Param {
    QString label;
    QString flag;
    QString value;
    QCheckBox *check = nullptr;
    QLineEdit *edit = nullptr;

    QStringList args() const {
        if (check && !check->isChecked()) return {};
        QString v = edit ? edit->text().trimmed() : value;
        if (v.isEmpty()) return {flag};
        return {flag, v};
    }
};

static QList<Param> makeInputParams() {
    return {
        {"RT Buffer Size", "-rtbufsize", "512M"},
        {"Device Timestamps", "-use_video_device_timestamps", "false"},
        {"Video Size", "-video_size", "720x576"},
        {"Framerate", "-framerate", "25"},
        {"Pixel Format", "-pixel_format", "yuyv422"},
        {"Sample Rate", "-sample_rate", "48000"},
    };
}

static QList<Param> makeCodecParams() {
    return {
        {"Video Codec", "-c:v", "libx264"},
        {"CRF", "-crf", "18"},
        {"Preset", "-preset", "fast"},
        {"Pixel Format", "-pix_fmt", "yuv420p"},
        {"Audio Codec", "-c:a", "pcm_s16le"},
    };
}

static QString timestampedFilename() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss") + ".mkv";
}

static QString sanitizeTapeName(QString s) {
    s = s.trimmed();
    s.replace(QRegularExpression(R"([^A-Za-z0-9._-]+)"), "_");
    s.remove(QRegularExpression(R"(^[._-]+|[._-]+$)"));
    return s;
}

static QString shellSingleQuote(QString s) {
    s.replace("'", "'\''");
    return "'" + s + "'";
}

static std::optional<qint64> parseDurationSeconds(const QString &s, QString *err = nullptr) {
    const auto parts = s.trimmed().split(':');
    bool ok1 = false, ok2 = false, ok3 = false;

    if (parts.size() == 3) {
        int h   = parts[0].trimmed().toInt(&ok1);
        int m   = parts[1].trimmed().toInt(&ok2);
        int sec = parts[2].trimmed().toInt(&ok3);

        if (!ok1 || !ok2 || !ok3) {
            if (err) *err = "Use HH:MM:SS";
            return std::nullopt;
        }
        return qint64(h) * 3600 + qint64(m) * 60 + sec;
    }

    if (parts.size() == 2) {
        int m   = parts[0].trimmed().toInt(&ok1);
        int sec = parts[1].trimmed().toInt(&ok2);

        if (!ok1 || !ok2) {
            if (err) *err = "Use MM:SS";
            return std::nullopt;
        }
        return qint64(m) * 60 + sec;
    }

    if (parts.size() == 1) {
        int sec = parts[0].trimmed().toInt(&ok1);
        if (!ok1) {
            if (err) *err = "Enter seconds as a plain number";
            return std::nullopt;
        }
        return sec;
    }

    if (err) *err = "Invalid duration format";
    return std::nullopt;
}

static QString hms(qint64 totalSeconds) {
    if (totalSeconds < 0) totalSeconds = 0;
    qint64 h = totalSeconds / 3600;
    qint64 m = (totalSeconds / 60) % 60;
    qint64 s = totalSeconds % 60;
    return QString("%1:%2:%3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() {
        inputParams = makeInputParams();
        codecParams = makeCodecParams();
        buildUi();
        refreshDevices();
        outLabel->setText(timestampedFilename());

        timer = new QTimer(this);
        timer->setInterval(1000);
        connect(timer, &QTimer::timeout, this, &MainWindow::onTimerTick);

        dvMonitorTimer = new QTimer(this);
        dvMonitorTimer->setInterval(2000);
        connect(dvMonitorTimer, &QTimer::timeout, this, &MainWindow::refreshDvgrabMonitor);

        resize(1200, 700);
        setWindowTitle("FFmpeg Capture Recorder + DV Grab");
    }

    ~MainWindow() override {
        stopPreview();
        stopRecording(true);
        stopDvgrabMonitor();
        stopDvgrab();
    }

private:
    QList<Param> inputParams;
    QList<Param> codecParams;

    QProcess *previewProc = nullptr;
    QProcess *recordProc = nullptr;

    QByteArray previewBuffer;
    QByteArray recordPreviewBuffer;
    QByteArray stderrBuffer;

    qint64 targetDurationSec = 0;
    QDateTime recordStart;
    QString currentOutputFile;

    QComboBox *videoSelect = nullptr;
    QComboBox *audioSelect = nullptr;
    QLineEdit *durEdit = nullptr;
    QLabel *outLabel = nullptr;
    QLabel *previewLabel = nullptr;
    QLabel *timerLabel = nullptr;
    QProgressBar *progress = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *previewBtn = nullptr;
    QPushButton *recordBtn = nullptr;
    QPlainTextEdit *logEdit = nullptr;
    QTimer *timer = nullptr;

    QLineEdit *sshTargetEdit = nullptr;
    QLineEdit *remoteBaseEdit = nullptr;
    QLineEdit *tapeNameEdit = nullptr;
    QLabel *remoteDirLabel = nullptr;
    QLabel *dvCmdPreview = nullptr;
    QLabel *dvStatusLabel = nullptr;
    QPushButton *dvStartBtn = nullptr;
    QPushButton *dvStopBtn = nullptr;
    QPushButton *dvAttachBtn = nullptr;
    QPushButton *dvRefreshMonitorBtn = nullptr;
    QPushButton *dvDetachMonitorBtn = nullptr;
    QPushButton *dvShutdownBtn = nullptr;
    QPlainTextEdit *dvMonitorEdit = nullptr;
    QTimer *dvMonitorTimer = nullptr;
    QProcess *dvgrabProc = nullptr;
    QProcess *dvMonitorProc = nullptr;
    QString currentDvSessionName;

    void buildUi() {
        auto *tabs = new QTabWidget(this);

        auto *recorderTab = new QWidget;
        auto *dvgrabTab = buildDvgrabTab();
        auto *advancedTab = buildAdvancedTab();
        auto *logTab = new QWidget;

        tabs->addTab(recorderTab, "Recorder");
        tabs->addTab(dvgrabTab, "DV Grab");
        tabs->addTab(advancedTab, "Advanced");
        tabs->addTab(logTab, "FFmpeg Log");
        setCentralWidget(tabs);

        videoSelect = new QComboBox;
        audioSelect = new QComboBox;
        durEdit = new QLineEdit("03:30:00");
        outLabel = new QLabel;
        outLabel->setWordWrap(true);

        connect(durEdit, &QLineEdit::returnPressed, this, [this] {
            QString err;
            auto sec = parseDurationSeconds(durEdit->text(), &err);
            if (!sec || *sec <= 0) {
                QMessageBox::critical(this, "Invalid duration", err);
                return;
            }
            targetDurationSec = *sec;
            if (recordProc) setStatus("Target updated -> " + durEdit->text().trimmed());
        });

        auto *refreshBtn = new QPushButton("Refresh Devices");
        connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshDevices);

        auto *form = new QFormLayout;
        form->addRow("Video Device", videoSelect);
        form->addRow("Audio Device", audioSelect);
        form->addRow("Duration", durEdit);
        form->addRow("Output File", outLabel);

        previewLabel = new QLabel;
        previewLabel->setMinimumSize(720, 576);
        previewLabel->setAlignment(Qt::AlignCenter);
        previewLabel->setFrameShape(QFrame::StyledPanel);
        previewLabel->setText("Preview");

        timerLabel = new QLabel;
        timerLabel->setAlignment(Qt::AlignCenter);
        timerLabel->setStyleSheet("font-weight: bold; font-family: monospace; padding: 4px;");
        timerLabel->hide();

        auto *previewBox = new QVBoxLayout;
        previewBox->addWidget(previewLabel, 1);
        previewBox->addWidget(timerLabel, 0);

        previewBtn = new QPushButton("Start Preview");
        recordBtn = new QPushButton("Start Recording");

        connect(previewBtn, &QPushButton::clicked, this, [this] {
            if (previewProc) stopPreview();
            else startPreview();
        });

        connect(recordBtn, &QPushButton::clicked, this, [this] {
            if (recordProc) stopRecording(false);
            else startRecording();
        });

        progress = new QProgressBar;
        progress->setRange(0, 1000);
        progress->setValue(0);

        statusLabel = new QLabel("Ready - select devices and press Start Preview.");

        auto *left = new QWidget;
        auto *leftLayout = new QVBoxLayout(left);
        leftLayout->addLayout(form);
        leftLayout->addWidget(refreshBtn);

        auto *btnRow = new QHBoxLayout;
        btnRow->addWidget(previewBtn);
        btnRow->addWidget(recordBtn);
        leftLayout->addLayout(btnRow);
        leftLayout->addWidget(progress);
        leftLayout->addWidget(statusLabel);
        leftLayout->addStretch();

        auto *right = new QWidget;
        right->setLayout(previewBox);

        auto *split = new QSplitter;
        split->addWidget(left);
        split->addWidget(right);
        split->setStretchFactor(1, 1);

        auto *recLayout = new QVBoxLayout(recorderTab);
        recLayout->addWidget(split);

        logEdit = new QPlainTextEdit;
        logEdit->setReadOnly(true);
        logEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        logEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

        auto *clearBtn = new QPushButton("Clear Log");
        connect(clearBtn, &QPushButton::clicked, this, [this] { logEdit->clear(); });

        auto *logLayout = new QVBoxLayout(logTab);
        logLayout->addWidget(clearBtn, 0, Qt::AlignRight);
        logLayout->addWidget(logEdit, 1);
    }

    QString buildDvgrabSessionName() const {
        const QString tape = sanitizeTapeName(tapeNameEdit ? tapeNameEdit->text() : QString());
        return tape.isEmpty() ? QString() : ("dvgrab-" + tape);
    }

    QString activeDvSessionName() const {
        return currentDvSessionName.isEmpty() ? buildDvgrabSessionName() : currentDvSessionName;
    }

    QString buildDvgrabInnerCommand() const {
        const QString tape = sanitizeTapeName(tapeNameEdit ? tapeNameEdit->text() : QString());
        const QString base = remoteBaseEdit ? remoteBaseEdit->text().trimmed() : QString();
        if (tape.isEmpty() || base.isEmpty()) return {};

        const QString dir = base + "/" + tape;
        const QString prefix = dir + "/" + tape + "-";
        return "mkdir -p " + shellSingleQuote(dir)
            + " && dvgrab -a -t --rewind --showstatus " + shellSingleQuote(prefix);
    }

    QString buildDvgrabRemoteCommand() const {
        const QString session = buildDvgrabSessionName();
        const QString inner = buildDvgrabInnerCommand();
        if (session.isEmpty() || inner.isEmpty()) return {};

        const QString sessionQ = shellSingleQuote(session);
        const QString innerQ = shellSingleQuote(inner);
        const QString logPathQ = shellSingleQuote("/tmp/" + session + ".log");

        return "if command -v byobu >/dev/null 2>&1; then "
               "byobu new-session -d -s " + sessionQ + " sh -lc " + innerQ +
               "; elif command -v tmux >/dev/null 2>&1; then "
               "tmux new-session -d -s " + sessionQ + " sh -lc " + innerQ +
               "; else nohup sh -lc " + innerQ + " >" + logPathQ + " 2>&1 < /dev/null & fi";
    }

    QString buildDvgrabMonitorCommand() const {
        const QString session = activeDvSessionName();
        if (session.isEmpty()) return {};

        const QString sessionQ = shellSingleQuote(session);
        const QString logPathQ = shellSingleQuote("/tmp/" + session + ".log");
        return "if command -v byobu >/dev/null 2>&1 && byobu has-session -t " + sessionQ + " 2>/dev/null; then "
               "byobu capture-pane -p -S -200 -t " + sessionQ +
               "; elif command -v tmux >/dev/null 2>&1 && tmux has-session -t " + sessionQ + " 2>/dev/null; then "
               "tmux capture-pane -p -S -200 -t " + sessionQ +
               "; elif test -f " + logPathQ + "; then tail -n 200 " + logPathQ +
               "; else echo '[session not found]'; exit 1; fi";
    }

    QString buildRemoteShutdownCommand() const {
        const QString inner =
            "sleep 1; if command -v systemctl >/dev/null 2>&1; then "
            "sudo -n systemctl poweroff || systemctl poweroff || sudo -n shutdown -h now || shutdown -h now; "
            "else sudo -n shutdown -h now || shutdown -h now; fi";
        return "nohup sh -lc " + shellSingleQuote(inner) + " >/tmp/remote-poweroff.log 2>&1 < /dev/null &";
    }

    void updateDvgrabPreview() {
        if (!sshTargetEdit || !remoteBaseEdit || !tapeNameEdit || !remoteDirLabel || !dvCmdPreview || !dvStartBtn) {
            return;
        }

        const QString tape = sanitizeTapeName(tapeNameEdit->text());
        const QString base = remoteBaseEdit->text().trimmed();
        const QString target = sshTargetEdit->text().trimmed();
        const QString session = buildDvgrabSessionName();
        const bool canAddressSession = !target.isEmpty() && !session.isEmpty();

        if (tape.isEmpty() || base.isEmpty()) {
            remoteDirLabel->setText("Remote Folder: -");
            dvCmdPreview->setText("Enter a tape name to build the detached dvgrab command.");
            dvStartBtn->setEnabled(false);
            if (dvAttachBtn) dvAttachBtn->setEnabled(false);
            if (dvRefreshMonitorBtn) dvRefreshMonitorBtn->setEnabled(false);
            if (dvDetachMonitorBtn) dvDetachMonitorBtn->setEnabled(dvMonitorTimer && dvMonitorTimer->isActive());
            if (dvShutdownBtn) dvShutdownBtn->setEnabled(!target.isEmpty());
            return;
        }

        const QString dir = base + "/" + tape;
        remoteDirLabel->setText("Remote Folder: " + dir + "    |    Session: " + session);

        const QString remoteCmd = buildDvgrabRemoteCommand();
        if (target.isEmpty()) {
            dvCmdPreview->setText(remoteCmd);
            dvStartBtn->setEnabled(false);
            if (dvAttachBtn) dvAttachBtn->setEnabled(false);
            if (dvRefreshMonitorBtn) dvRefreshMonitorBtn->setEnabled(false);
            if (dvDetachMonitorBtn) dvDetachMonitorBtn->setEnabled(dvMonitorTimer && dvMonitorTimer->isActive());
            if (dvShutdownBtn) dvShutdownBtn->setEnabled(false);
            return;
        }

        dvCmdPreview->setText("ssh " + target + " " + remoteCmd);
        dvStartBtn->setEnabled(dvgrabProc == nullptr);
        if (dvAttachBtn) dvAttachBtn->setEnabled(canAddressSession);
        if (dvRefreshMonitorBtn) dvRefreshMonitorBtn->setEnabled(canAddressSession && dvMonitorProc == nullptr);
        if (dvDetachMonitorBtn) dvDetachMonitorBtn->setEnabled(dvMonitorTimer && dvMonitorTimer->isActive());
        if (dvShutdownBtn) dvShutdownBtn->setEnabled(true);
    }

    QWidget* buildDvgrabTab() {
        auto *tab = new QWidget;

        sshTargetEdit = new QLineEdit("uni@recorder");
        remoteBaseEdit = new QLineEdit("/mnt/data");
        tapeNameEdit = new QLineEdit;
        tapeNameEdit->setPlaceholderText("Tape name, e.g. holiday-2007-01");

        remoteDirLabel = new QLabel("Remote Folder: -");
        remoteDirLabel->setWordWrap(true);

        dvCmdPreview = new QLabel;
        dvCmdPreview->setWordWrap(true);
        dvCmdPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
        dvCmdPreview->setFrameShape(QFrame::StyledPanel);
        dvCmdPreview->setMargin(8);

        auto *form = new QFormLayout;
        form->addRow("SSH Target", sshTargetEdit);
        form->addRow("Remote Base Path", remoteBaseEdit);
        form->addRow("Tape Name", tapeNameEdit);
        form->addRow("Destination", remoteDirLabel);
        form->addRow("Command", dvCmdPreview);

        auto *hint = new QLabel(
            "This panel connects over SSH, creates /mnt/data/<tapename>, starts dvgrab in a detached byobu or tmux session, "
            "can monitor the running session inside the app, and can shut down the remote recorder when you are done."
        );
        hint->setWordWrap(true);
        hint->setStyleSheet("color: palette(mid);");

        dvStartBtn = new QPushButton("Start Detached DV Capture");
        dvStopBtn = new QPushButton("Stop Remote Session");
        dvAttachBtn = new QPushButton("Attach / Monitor Session");
        dvRefreshMonitorBtn = new QPushButton("Refresh Snapshot");
        dvDetachMonitorBtn = new QPushButton("Stop Monitoring");
        dvShutdownBtn = new QPushButton("Shut Down Remote Machine");

        dvStopBtn->setEnabled(false);
        dvDetachMonitorBtn->setEnabled(false);

        dvStatusLabel = new QLabel("Ready - enter a tape name to prepare the detached capture session.");
        dvStatusLabel->setWordWrap(true);

        auto *btnRow = new QHBoxLayout;
        btnRow->addWidget(dvStartBtn);
        btnRow->addWidget(dvStopBtn);
        btnRow->addWidget(dvAttachBtn);
        btnRow->addWidget(dvRefreshMonitorBtn);
        btnRow->addWidget(dvDetachMonitorBtn);
        btnRow->addWidget(dvShutdownBtn);
        btnRow->addStretch();

        dvMonitorEdit = new QPlainTextEdit;
        dvMonitorEdit->setReadOnly(true);
        dvMonitorEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        dvMonitorEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        dvMonitorEdit->setPlaceholderText("Session output will appear here after you attach to the detached byobu/tmux session.");
        dvMonitorEdit->setMinimumHeight(260);

        auto *monitorBox = new QGroupBox("Session Monitor");
        auto *monitorLayout = new QVBoxLayout(monitorBox);
        monitorLayout->addWidget(dvMonitorEdit);

        auto *layout = new QVBoxLayout(tab);
        layout->addWidget(hint);
        layout->addLayout(form);
        layout->addLayout(btnRow);
        layout->addWidget(dvStatusLabel);
        layout->addWidget(monitorBox, 1);

        connect(sshTargetEdit, &QLineEdit::textChanged, this, [this] { updateDvgrabPreview(); });
        connect(remoteBaseEdit, &QLineEdit::textChanged, this, [this] { updateDvgrabPreview(); });
        connect(tapeNameEdit, &QLineEdit::textChanged, this, [this] { updateDvgrabPreview(); });
        connect(tapeNameEdit, &QLineEdit::returnPressed, this, [this] {
            if (dvStartBtn && dvStartBtn->isEnabled()) startDvgrab();
        });
        connect(dvStartBtn, &QPushButton::clicked, this, &MainWindow::startDvgrab);
        connect(dvStopBtn, &QPushButton::clicked, this, &MainWindow::stopDvgrab);
        connect(dvAttachBtn, &QPushButton::clicked, this, &MainWindow::startDvgrabMonitor);
        connect(dvRefreshMonitorBtn, &QPushButton::clicked, this, &MainWindow::refreshDvgrabMonitor);
        connect(dvDetachMonitorBtn, &QPushButton::clicked, this, &MainWindow::stopDvgrabMonitor);
        connect(dvShutdownBtn, &QPushButton::clicked, this, &MainWindow::shutdownRemoteMachine);

        updateDvgrabPreview();
        return tab;
    }

    QWidget* buildAdvancedTab() {
        auto *root = new QWidget;
        auto *layout = new QVBoxLayout(root);

        layout->addWidget(buildParamSection("Input / Capture Parameters", inputParams));
        layout->addWidget(buildParamSection("Output Codec Parameters", codecParams));

        auto *resetBtn = new QPushButton("Reset to Defaults");
        connect(resetBtn, &QPushButton::clicked, this, [this] {
            auto defInput = makeInputParams();
            auto defCodec = makeCodecParams();
            for (int i = 0; i < inputParams.size(); ++i) {
                inputParams[i].check->setChecked(true);
                inputParams[i].edit->setText(defInput[i].value);
            }
            for (int i = 0; i < codecParams.size(); ++i) {
                codecParams[i].check->setChecked(true);
                codecParams[i].edit->setText(defCodec[i].value);
            }
        });

        layout->addWidget(resetBtn);
        layout->addStretch();

        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(root);

        auto *wrapper = new QWidget;
        auto *w = new QVBoxLayout(wrapper);
        w->addWidget(scroll);
        return wrapper;
    }

    QWidget* buildParamSection(const QString &title, QList<Param> &params) {
        auto *box = new QGroupBox(title);
        auto *form = new QFormLayout(box);

        for (auto &p : params) {
            p.check = new QCheckBox;
            p.check->setChecked(true);
            p.edit = new QLineEdit(p.value);

            auto *row = new QWidget;
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0,0,0,0);
            rowLayout->addWidget(p.check);
            rowLayout->addWidget(p.edit, 1);

            form->addRow(p.label + "\n" + p.flag, row);
        }
        return box;
    }

    QStringList collectArgs(const QList<Param> &params, const QSet<QString> &exclude = {}) const {
        QStringList out;
        for (const auto &p : params) {
            if (exclude.contains(p.flag)) continue;
            out += p.args();
        }
        return out;
    }

    void refreshDevices() {
        QProcess p;
        hideChildConsole(p);
        p.start("ffmpeg", {"-list_devices", "true", "-f", "dshow", "-i", "dummy"});
        p.waitForFinished(10000);

        const QString text = QString::fromLocal8Bit(p.readAllStandardError()) +
                             QString::fromLocal8Bit(p.readAllStandardOutput());

        QRegularExpression re("\"([^\"]+)\"\\s+\\((video|audio)\\)");
        QSet<QString> vids, auds;

        for (const QString &line : text.split('\n')) {
            if (line.contains("Alternative name")) continue;
            auto m = re.match(line);
            if (!m.hasMatch()) continue;
            if (m.captured(2) == "video") vids.insert(m.captured(1));
            if (m.captured(2) == "audio") auds.insert(m.captured(1));
        }

        videoSelect->clear();
        audioSelect->clear();

        if (vids.isEmpty()) videoSelect->addItem("(no video devices found)");
        else {
            auto list = vids.values();
            std::sort(list.begin(), list.end());
            videoSelect->addItems(list);
        }

        if (auds.isEmpty()) audioSelect->addItem("(no audio devices found)");
        else {
            auto list = auds.values();
            std::sort(list.begin(), list.end());
            audioSelect->addItems(list);
        }

        selectPreferred(videoSelect, "USB Video");
        selectPreferred(audioSelect, "Digital Audio Interface (USB Digital Audio)");
    }

    void selectPreferred(QComboBox *box, const QString &preferred) {
        int idx = box->findText(preferred);
        if (idx >= 0) box->setCurrentIndex(idx);
        else if (box->count() > 0) box->setCurrentIndex(0);
    }

    void startPreview() {
        stopPreview();

        const QString vDev = videoSelect->currentText();
        if (vDev.isEmpty() || vDev.startsWith("(")) {
            setStatus("No valid video device selected.");
            return;
        }

        previewProc = new QProcess(this);
        hideChildConsole(*previewProc);
        previewBuffer.clear();

        connect(previewProc, &QProcess::readyReadStandardOutput, this, [this] {
            previewBuffer += previewProc->readAllStandardOutput();
            consumeMjpeg(previewBuffer);
        });

        connect(previewProc, &QProcess::readyReadStandardError, this, [this] {
            appendLog(QString::fromLocal8Bit(previewProc->readAllStandardError()));
        });

        connect(previewProc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int, QProcess::ExitStatus) {
            previewProc->deleteLater();
            previewProc = nullptr;
            previewBtn->setText("Start Preview");
            setStatus("Preview stopped.");
        });

        QStringList args = {"-f", "dshow"};
        args += collectArgs(inputParams, {"-sample_rate"});
        args += {"-i", "video=" + vDev,
                 "-vf", "scale=720:576",
                 "-f", "mjpeg",
                 "-q:v", "5",
                 "-r", "10",
                 "pipe:1"};

        previewProc->start("ffmpeg", args);
        if (!previewProc->waitForStarted(5000)) {
            setStatus("Preview start error: " + previewProc->errorString());
            previewProc->deleteLater();
            previewProc = nullptr;
            return;
        }

        previewBtn->setText("Stop Preview");
        setStatus("Preview running...");
    }

    void stopPreview() {
        if (!previewProc) return;
        previewProc->kill();
        previewProc->waitForFinished(3000);
    }

    void startRecording() {
        QString err;
        auto sec = parseDurationSeconds(durEdit->text(), &err);
        if (!sec || *sec <= 0) {
            QMessageBox::critical(this, "Invalid duration", err);
            return;
        }

        targetDurationSec = *sec;
        currentOutputFile = timestampedFilename();
        outLabel->setText(currentOutputFile);

        const QString vDev = videoSelect->currentText();
        const QString aDev = audioSelect->currentText();

        stopPreview();
        previewBtn->setEnabled(false);

        recordProc = new QProcess(this);
        hideChildConsole(*recordProc);

        previewBuffer.clear();
        stderrBuffer.clear();
        logEdit->clear();

        connect(recordProc, &QProcess::readyReadStandardOutput, this, [this] {
            previewBuffer += recordProc->readAllStandardOutput();
            consumeMjpeg(previewBuffer);
        });

        connect(recordProc, &QProcess::readyReadStandardError, this, [this] {
            stderrBuffer += recordProc->readAllStandardError();
            consumeStderr();
        });

        connect(recordProc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int, QProcess::ExitStatus) {
            timer->stop();
            timerLabel->hide();

            recordProc->deleteLater();
            recordProc = nullptr;

            progress->setValue(1000);
            statusLabel->setText("Done: " + currentOutputFile);
            recordBtn->setText("Start Recording");
            previewBtn->setEnabled(true);
            outLabel->setText(timestampedFilename());
        });

        QStringList args = {"-f", "dshow"};
        args += collectArgs(inputParams);
        args += {"-i", QString("video=%1:audio=%2").arg(vDev, aDev)};

        args += {"-map", "0:v", "-map", "0:a"};
        args += collectArgs(codecParams);
        args += {currentOutputFile};

        args += {"-map", "0:v",
                 "-vf", "scale=720:576",
                 "-f", "mjpeg",
                 "-q:v", "5",
                 "-r", "10",
                 "pipe:1"};

        recordProc->start("ffmpeg", args);
        if (!recordProc->waitForStarted(5000)) {
            setStatus("Failed to start FFmpeg: " + recordProc->errorString());
            previewBtn->setEnabled(true);
            recordProc->deleteLater();
            recordProc = nullptr;
            return;
        }

        recordStart = QDateTime::currentDateTime();
        timer->start();

        progress->setValue(0);
        recordBtn->setText("Stop Recording");
        setStatus("Recording...");
    }

    void stopRecording(bool force) {
        if (!recordProc) return;

        if (!force) {
            recordProc->write("q\n");
            if (recordProc->waitForFinished(3000)) return;
        }

        recordProc->kill();
        recordProc->waitForFinished(3000);

        timer->stop();
        timerLabel->hide();
        recordBtn->setText("Start Recording");
        previewBtn->setEnabled(true);
        setStatus("Recording stopped by user.");
        outLabel->setText(timestampedFilename());
    }

    void startDvgrab() {
        if (dvgrabProc) return;
        if (!sshTargetEdit || !remoteBaseEdit || !tapeNameEdit || !dvStatusLabel || !dvStartBtn || !dvStopBtn) return;

        const QString target = sshTargetEdit->text().trimmed();
        const QString tape = sanitizeTapeName(tapeNameEdit->text());
        const QString remoteCmd = buildDvgrabRemoteCommand();
        const QString session = buildDvgrabSessionName();

        if (target.isEmpty()) {
            QMessageBox::warning(this, "SSH Target", "Please enter an SSH target, for example uni@recorder.");
            return;
        }
        if (tape.isEmpty()) {
            QMessageBox::warning(this, "Tape Name", "Please enter a tape name.");
            return;
        }
        if (remoteCmd.isEmpty()) {
            QMessageBox::warning(this, "Remote Path", "Please enter a remote base path.");
            return;
        }

        currentDvSessionName = session;
        logEdit->clear();
        appendLog("Launching detached remote dvgrab for tape '" + tape + "' in session '" + session + "'.");

        dvgrabProc = new QProcess(this);
        hideChildConsole(*dvgrabProc);

        connect(dvgrabProc, &QProcess::readyReadStandardOutput, this, [this] {
            appendLog(QString::fromLocal8Bit(dvgrabProc->readAllStandardOutput()));
        });
        connect(dvgrabProc, &QProcess::readyReadStandardError, this, [this] {
            appendLog(QString::fromLocal8Bit(dvgrabProc->readAllStandardError()));
        });
        connect(dvgrabProc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            const bool ok = (status == QProcess::NormalExit && code == 0);
            if (ok) {
                dvStatusLabel->setText("Detached capture started in session '" + currentDvSessionName + "'. Use Attach / Monitor Session to follow progress in the app.");
                dvStopBtn->setEnabled(true);
                QTimer::singleShot(0, this, &MainWindow::startDvgrabMonitor);
            } else {
                dvStatusLabel->setText(QString("Failed to launch detached DV capture (exit code %1).").arg(code));
                dvStopBtn->setEnabled(false);
            }
            dvStartBtn->setEnabled(true);
            dvgrabProc->deleteLater();
            dvgrabProc = nullptr;
            updateDvgrabPreview();
        });

        dvStatusLabel->setText("Connecting to recorder and launching detached dvgrab session...");
        dvStartBtn->setEnabled(false);
        dvStopBtn->setEnabled(false);

        dvgrabProc->start("ssh", {target, remoteCmd});
        if (!dvgrabProc->waitForStarted(5000)) {
            dvStatusLabel->setText("Failed to start SSH: " + dvgrabProc->errorString());
            dvStartBtn->setEnabled(true);
            dvgrabProc->deleteLater();
            dvgrabProc = nullptr;
            updateDvgrabPreview();
            return;
        }
    }

    void startDvgrabMonitor() {
        if (!sshTargetEdit || !dvStatusLabel || !dvMonitorEdit || !dvMonitorTimer) return;

        const QString target = sshTargetEdit->text().trimmed();
        const QString session = activeDvSessionName();
        if (target.isEmpty()) {
            QMessageBox::warning(this, "SSH Target", "Please enter an SSH target before attaching to the session.");
            return;
        }
        if (session.isEmpty()) {
            QMessageBox::warning(this, "Session", "Please enter a tape name so the session can be derived.");
            return;
        }

        currentDvSessionName = session;
        dvMonitorEdit->setPlainText("Attaching monitor to session '" + session + "'...");
        dvStatusLabel->setText("Monitoring session '" + session + "' inside the app.");
        dvMonitorTimer->start();
        refreshDvgrabMonitor();
        updateDvgrabPreview();
    }

    void refreshDvgrabMonitor() {
        if (dvMonitorProc || !sshTargetEdit || !dvMonitorEdit) return;

        const QString target = sshTargetEdit->text().trimmed();
        const QString session = activeDvSessionName();
        const QString monitorCmd = buildDvgrabMonitorCommand();
        if (target.isEmpty() || session.isEmpty() || monitorCmd.isEmpty()) {
            updateDvgrabPreview();
            return;
        }

        dvMonitorProc = new QProcess(this);
        hideChildConsole(*dvMonitorProc);

        connect(dvMonitorProc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, session](int code, QProcess::ExitStatus status) {
            QString textOut = QString::fromLocal8Bit(dvMonitorProc->readAllStandardOutput());
            const QString textErr = QString::fromLocal8Bit(dvMonitorProc->readAllStandardError());
            if (!textErr.trimmed().isEmpty()) {
                if (!textOut.isEmpty() && !textOut.endsWith('
')) textOut += '
';
                textOut += textErr;
            }
            if (textOut.trimmed().isEmpty()) textOut = "[no output yet]";
            dvMonitorEdit->setPlainText(textOut);
            auto *bar = dvMonitorEdit->verticalScrollBar();
            bar->setValue(bar->maximum());

            const bool ok = (status == QProcess::NormalExit && code == 0);
            if (!ok && dvMonitorTimer && dvMonitorTimer->isActive()) {
                dvMonitorTimer->stop();
                dvStatusLabel->setText("Monitor lost session '" + session + "'.");
            }

            dvMonitorProc->deleteLater();
            dvMonitorProc = nullptr;
            updateDvgrabPreview();
        });

        dvMonitorProc->start("ssh", {target, monitorCmd});
        if (!dvMonitorProc->waitForStarted(5000)) {
            dvStatusLabel->setText("Failed to start monitor SSH: " + dvMonitorProc->errorString());
            dvMonitorProc->deleteLater();
            dvMonitorProc = nullptr;
            updateDvgrabPreview();
        }
    }

    void stopDvgrabMonitor() {
        if (dvMonitorTimer) dvMonitorTimer->stop();
        if (dvMonitorProc) {
            dvMonitorProc->kill();
            dvMonitorProc->waitForFinished(3000);
            dvMonitorProc->deleteLater();
            dvMonitorProc = nullptr;
        }
        if (dvStatusLabel && !activeDvSessionName().isEmpty()) {
            dvStatusLabel->setText("Stopped monitoring session '" + activeDvSessionName() + "'.");
        }
        updateDvgrabPreview();
    }

    void stopDvgrab() {
        if (dvgrabProc) {
            dvgrabProc->terminate();
            if (!dvgrabProc->waitForFinished(3000)) {
                dvgrabProc->kill();
                dvgrabProc->waitForFinished(3000);
            }
            return;
        }

        if (!sshTargetEdit || !dvStatusLabel || activeDvSessionName().isEmpty()) return;

        const QString target = sshTargetEdit->text().trimmed();
        if (target.isEmpty()) return;

        stopDvgrabMonitor();

        const QString session = activeDvSessionName();
        const QString sessionQ = shellSingleQuote(session);
        const QString stopCmd =
            "if command -v byobu >/dev/null 2>&1; then byobu kill-session -t " + sessionQ +
            "; elif command -v tmux >/dev/null 2>&1; then tmux kill-session -t " + sessionQ +
            "; else pkill -f " + shellSingleQuote("dvgrab -a -t --rewind --showstatus") + "; fi";

        QProcess p;
        hideChildConsole(p);
        dvStatusLabel->setText("Stopping detached DV session '" + session + "'...");
        appendLog("Stopping detached DV session '" + session + "'.");
        p.start("ssh", {target, stopCmd});
        if (!p.waitForFinished(10000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
            appendLog(QString::fromLocal8Bit(p.readAllStandardError()));
            appendLog(QString::fromLocal8Bit(p.readAllStandardOutput()));
            dvStatusLabel->setText("Failed to stop detached DV session.");
            return;
        }

        appendLog(QString::fromLocal8Bit(p.readAllStandardError()));
        appendLog(QString::fromLocal8Bit(p.readAllStandardOutput()));
        dvStatusLabel->setText("Detached DV session stopped: " + session);
        currentDvSessionName.clear();
        if (dvMonitorEdit) dvMonitorEdit->setPlainText("[session stopped]");
        dvStopBtn->setEnabled(false);
        updateDvgrabPreview();
    }

    void shutdownRemoteMachine() {
        if (!sshTargetEdit || !dvStatusLabel) return;

        const QString target = sshTargetEdit->text().trimmed();
        if (target.isEmpty()) {
            QMessageBox::warning(this, "SSH Target", "Please enter an SSH target before shutting down the remote machine.");
            return;
        }

        const auto answer = QMessageBox::warning(
            this,
            "Shut Down Remote Machine",
            "This will power off the remote recorder and end any running capture session. Continue?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (answer != QMessageBox::Yes) return;

        stopDvgrabMonitor();

        QProcess p;
        hideChildConsole(p);
        appendLog("Requesting remote shutdown on '" + target + "'.");
        p.start("ssh", {target, buildRemoteShutdownCommand()});
        if (!p.waitForFinished(10000)) {
            appendLog(QString::fromLocal8Bit(p.readAllStandardError()));
            appendLog(QString::fromLocal8Bit(p.readAllStandardOutput()));
            dvStatusLabel->setText("Shutdown request sent; the remote machine may already be powering off.");
        } else if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) {
            appendLog(QString::fromLocal8Bit(p.readAllStandardError()));
            appendLog(QString::fromLocal8Bit(p.readAllStandardOutput()));
            dvStatusLabel->setText("Remote shutdown command sent successfully.");
        } else {
            appendLog(QString::fromLocal8Bit(p.readAllStandardError()));
            appendLog(QString::fromLocal8Bit(p.readAllStandardOutput()));
            dvStatusLabel->setText("Remote shutdown command returned a non-zero exit code.");
        }

        currentDvSessionName.clear();
        if (dvMonitorEdit) dvMonitorEdit->setPlainText("[remote shutdown requested]");
        if (dvStopBtn) dvStopBtn->setEnabled(false);
        updateDvgrabPreview();
    }

    void onTimerTick() {
        if (!recordProc) {
            timer->stop();
            timerLabel->hide();
            return;
        }

        qint64 elapsed = recordStart.secsTo(QDateTime::currentDateTime());
        if (elapsed >= targetDurationSec) {
            timerLabel->hide();
            stopRecording(false);
            return;
        }

        qint64 remaining = targetDurationSec - elapsed;
        timerLabel->setText(" " + hms(elapsed) + " / -" + hms(remaining) + " ");
        timerLabel->show();
    }

    void consumeMjpeg(QByteArray &buf) {
        static const QByteArray soi("\xFF\xD8", 2);
        static const QByteArray eoi("\xFF\xD9", 2);

        while (true) {
            int start = buf.indexOf(soi);
            if (start < 0) {
                buf.clear();
                return;
            }

            int end = buf.indexOf(eoi, start + 2);
            if (end < 0) {
                if (start > 0) buf.remove(0, start);
                return;
            }

            end += 2;
            QByteArray frame = buf.mid(start, end - start);
            buf.remove(0, end);

            QPixmap px;
            if (px.loadFromData(frame, "JPG")) {
                previewLabel->setPixmap(px.scaled(previewLabel->size(),
                                                  Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation));
            }
        }
    }

    void consumeStderr() {
        auto processLine = [this](const QString &lineRaw) {
            QString line = lineRaw.trimmed();
            if (line.isEmpty()) return;

            static QRegularExpression progressRe(R"(^frame=\s*\d+)");
            static QRegularExpression timeRe(R"(time=(\d+):(\d+):(\d+)\.(\d+))");

            bool isProgressLine = progressRe.match(line).hasMatch();

            // Only send non-progress lines to the log
            if (!isProgressLine) {
                appendLog(line);
            }

            if (isProgressLine) {
                auto m = timeRe.match(line);
                if (m.hasMatch()) {
                    qint64 h   = m.captured(1).toLongLong();
                    qint64 min = m.captured(2).toLongLong();
                    qint64 sec = m.captured(3).toLongLong();
                    qint64 elapsed = h * 3600 + min * 60 + sec;

                    double pct = targetDurationSec > 0
                        ? std::min(1.0, double(elapsed) / double(targetDurationSec))
                        : 0.0;

                    progress->setValue(int(pct * 1000.0));
                    statusLabel->setText(
                        QString("● Recording %1 / %2").arg(hms(elapsed), hms(targetDurationSec))
                    );
                }
            }
        };

        while (true) {
            int cr = stderrBuffer.indexOf('\r');
            int lf = stderrBuffer.indexOf('\n');
            int pos = -1;

            if (cr >= 0 && lf >= 0) pos = std::min(cr, lf);
            else if (cr >= 0) pos = cr;
            else if (lf >= 0) pos = lf;
            else break;

            QByteArray line = stderrBuffer.left(pos);
            stderrBuffer.remove(0, pos + 1);
            if (!line.isEmpty() && line.endsWith('\r')) line.chop(1);
            processLine(QString::fromLocal8Bit(line));
        }
    }

    void appendLog(const QString &s) {
        logEdit->appendPlainText(s);
        auto *bar = logEdit->verticalScrollBar();
        bar->setValue(bar->maximum());
    }

    void setStatus(const QString &s) {
        statusLabel->setText(s);
    }

    void hideChildConsole(QProcess &proc) {
#ifdef Q_OS_WIN
        proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
            args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            args->startupInfo->wShowWindow = SW_HIDE;
        });
#else
        Q_UNUSED(proc);
#endif
    }
};

#include "main.moc"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    MainWindow w;
    w.show();
    return app.exec();
}
