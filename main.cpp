#include <QtWidgets>
#include <QtCore>
#include <QtGui>
#include <optional>

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

struct DvStatus {
    QString file;
    double mib = 0.0;
    qint64 frames = 0;
    QString timecode;
    QString recordedAt;
    bool valid = false;
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
    s.replace("'", "'\\''");
    return "'" + s + "'";
}

static std::optional<qint64> parseDurationSeconds(const QString &s, QString *err = nullptr) {
    const auto parts = s.trimmed().split(':');
    bool ok1 = false, ok2 = false, ok3 = false;

    if (parts.size() == 3) {
        int h = parts[0].trimmed().toInt(&ok1);
        int m = parts[1].trimmed().toInt(&ok2);
        int sec = parts[2].trimmed().toInt(&ok3);
        if (!ok1 || !ok2 || !ok3) {
            if (err) *err = "Use HH:MM:SS";
            return std::nullopt;
        }
        return qint64(h) * 3600 + qint64(m) * 60 + sec;
    }

    if (parts.size() == 2) {
        int m = parts[0].trimmed().toInt(&ok1);
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

        resize(1280, 760);
        setWindowTitle("FFmpeg Capture Recorder");
    }

    ~MainWindow() override {
        stopPreview();
        stopRecording(true);
        stopDvgrabMonitor();
        if (dvLaunchProc) {
            dvLaunchProc->kill();
            dvLaunchProc->waitForFinished(1000);
        }
    }

private:
    QList<Param> inputParams;
    QList<Param> codecParams;

    QProcess *previewProc = nullptr;
    QProcess *recordProc = nullptr;
    QProcess *dvLaunchProc = nullptr;
    QProcess *dvMonitorProc = nullptr;

    QByteArray previewBuffer;
    QByteArray stderrBuffer;

    qint64 targetDurationSec = 0;
    QDateTime recordStart;
    QString currentOutputFile;
    QString currentDvSessionName;
    QString lastDvStatusKey;
    QString lastDvFile;
    QStringList recentDvEvents;

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
    QLabel *dvRemoteInfoLabel = nullptr;
    QLabel *dvCmdPreview = nullptr;
    QLabel *dvStatusLabel = nullptr;
    QLabel *dvLiveLabel = nullptr;
    QPlainTextEdit *dvEventLogEdit = nullptr;
    QPlainTextEdit *dvVerboseEdit = nullptr;
    QCheckBox *dvVerboseCheck = nullptr;
    QPushButton *dvStartBtn = nullptr;
    QPushButton *dvStopBtn = nullptr;
    QPushButton *dvAttachBtn = nullptr;
    QPushButton *dvDetachBtn = nullptr;
    QPushButton *dvShutdownBtn = nullptr;
    QTimer *dvMonitorTimer = nullptr;

    void buildUi() {
        auto *tabs = new QTabWidget(this);
        auto *recorderTab = new QWidget;
        auto *dvTab = buildDvgrabTab();
        auto *advancedTab = buildAdvancedTab();
        auto *logTab = new QWidget;

        tabs->addTab(recorderTab, "Recorder");
        tabs->addTab(dvTab, "DV Grab");
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

    QWidget* buildDvgrabTab() {
        auto *tab = new QWidget;

        sshTargetEdit = new QLineEdit("uni@recorder");
        remoteBaseEdit = new QLineEdit("/mnt/data");
        tapeNameEdit = new QLineEdit;
        tapeNameEdit->setPlaceholderText("e.g. testing or holiday-2007-01");

        dvRemoteInfoLabel = new QLabel("Destination: -");
        dvRemoteInfoLabel->setWordWrap(true);

        dvCmdPreview = new QLabel;
        dvCmdPreview->setWordWrap(true);
        dvCmdPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
        dvCmdPreview->setFrameShape(QFrame::StyledPanel);
        dvCmdPreview->setMargin(8);

        auto *form = new QFormLayout;
        form->addRow("SSH Target", sshTargetEdit);
        form->addRow("Remote Base Path", remoteBaseEdit);
        form->addRow("Tape Name", tapeNameEdit);
        form->addRow("Destination", dvRemoteInfoLabel);
        form->addRow("Detached Command", dvCmdPreview);

        auto *hint = new QLabel(
            "The recorder launches dvgrab in a detached byobu/tmux session. "
            "The monitor stays quiet by keeping only the latest live status and logging only important events."
        );
        hint->setWordWrap(true);
        hint->setStyleSheet("color: palette(mid);");

        dvStartBtn = new QPushButton("Start Detached DV Capture");
        dvStopBtn = new QPushButton("Stop Remote Session");
        dvAttachBtn = new QPushButton("Attach / Monitor");
        dvDetachBtn = new QPushButton("Stop Monitoring");
        dvShutdownBtn = new QPushButton("Shut Down Remote Machine");
        dvVerboseCheck = new QCheckBox("Show raw monitor tail");

        dvLiveLabel = new QLabel("No active DV status yet.");
        dvLiveLabel->setWordWrap(true);
        dvLiveLabel->setFrameShape(QFrame::StyledPanel);
        dvLiveLabel->setMargin(10);
        dvLiveLabel->setMinimumHeight(120);
        dvLiveLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        dvStatusLabel = new QLabel("Ready - enter a tape name to prepare the detached DV session.");
        dvStatusLabel->setWordWrap(true);

        dvEventLogEdit = new QPlainTextEdit;
        dvEventLogEdit->setReadOnly(true);
        dvEventLogEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        dvEventLogEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        dvEventLogEdit->setPlaceholderText("Important DV events will appear here.");

        dvVerboseEdit = new QPlainTextEdit;
        dvVerboseEdit->setReadOnly(true);
        dvVerboseEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        dvVerboseEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        dvVerboseEdit->setPlaceholderText("Optional raw session tail.");
        dvVerboseEdit->setVisible(false);

        auto *buttonRow = new QHBoxLayout;
        buttonRow->addWidget(dvStartBtn);
        buttonRow->addWidget(dvStopBtn);
        buttonRow->addWidget(dvAttachBtn);
        buttonRow->addWidget(dvDetachBtn);
        buttonRow->addWidget(dvShutdownBtn);
        buttonRow->addStretch();

        auto *liveBox = new QGroupBox("Current Capture");
        auto *liveLayout = new QVBoxLayout(liveBox);
        liveLayout->addWidget(dvLiveLabel);

        auto *eventBox = new QGroupBox("Important Events");
        auto *eventLayout = new QVBoxLayout(eventBox);
        eventLayout->addWidget(dvEventLogEdit);

        auto *verboseBox = new QGroupBox("Raw Tail");
        auto *verboseLayout = new QVBoxLayout(verboseBox);
        verboseLayout->addWidget(dvVerboseCheck);
        verboseLayout->addWidget(dvVerboseEdit);

        auto *layout = new QVBoxLayout(tab);
        layout->addWidget(hint);
        layout->addLayout(form);
        layout->addLayout(buttonRow);
        layout->addWidget(dvStatusLabel);
        layout->addWidget(liveBox);
        layout->addWidget(eventBox, 1);
        layout->addWidget(verboseBox);

        dvStopBtn->setEnabled(false);
        dvDetachBtn->setEnabled(false);

        connect(sshTargetEdit, &QLineEdit::textChanged, this, &MainWindow::updateDvgrabPreview);
        connect(remoteBaseEdit, &QLineEdit::textChanged, this, &MainWindow::updateDvgrabPreview);
        connect(tapeNameEdit, &QLineEdit::textChanged, this, &MainWindow::updateDvgrabPreview);
        connect(tapeNameEdit, &QLineEdit::returnPressed, this, [this] {
            if (dvStartBtn->isEnabled()) startDvgrab();
        });
        connect(dvStartBtn, &QPushButton::clicked, this, &MainWindow::startDvgrab);
        connect(dvStopBtn, &QPushButton::clicked, this, &MainWindow::stopDvgrabSession);
        connect(dvAttachBtn, &QPushButton::clicked, this, &MainWindow::startDvgrabMonitor);
        connect(dvDetachBtn, &QPushButton::clicked, this, &MainWindow::stopDvgrabMonitor);
        connect(dvShutdownBtn, &QPushButton::clicked, this, &MainWindow::shutdownRemoteMachine);
        connect(dvVerboseCheck, &QCheckBox::toggled, this, [this](bool on) {
            dvVerboseEdit->setVisible(on);
        });

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
            rowLayout->setContentsMargins(0, 0, 0, 0);
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

        const QString text = QString::fromLocal8Bit(p.readAllStandardError()) + QString::fromLocal8Bit(p.readAllStandardOutput());
        QRegularExpression re(QStringLiteral("\"([^\"]+)\"\\s+\\((video|audio)\\)"));
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

        if (vids.isEmpty()) videoSelect->addItem("<no video devices found>");
        else {
            auto list = vids.values();
            std::sort(list.begin(), list.end());
            videoSelect->addItems(list);
        }

        if (auds.isEmpty()) audioSelect->addItem("<no audio devices found>");
        else {
            auto list = auds.values();
            std::sort(list.begin(), list.end());
            audioSelect->addItems(list);
        }

        selectPreferred(videoSelect, "USB Video");
        selectPreferred(audioSelect, "Digital Audio Interface (USB Digital Audio)");
        appendLog("Refreshed FFmpeg dshow devices.");
    }

    void selectPreferred(QComboBox *box, const QString &preferred) {
        int idx = box->findText(preferred);
        if (idx >= 0) box->setCurrentIndex(idx);
        else if (box->count() > 0) box->setCurrentIndex(0);
    }

    void startPreview() {
        stopPreview();
        const QString vDev = videoSelect->currentText();
        if (vDev.isEmpty() || vDev.startsWith('<')) {
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
        connect(previewProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int, QProcess::ExitStatus) {
            previewProc->deleteLater();
            previewProc = nullptr;
            previewBtn->setText("Start Preview");
            setStatus("Preview stopped.");
        });

        QStringList args = {"-f", "dshow"};
        args += collectArgs(inputParams, {"-sample_rate"});
        args += {"-i", "video=" + vDev, "-vf", "scale=720:576", "-f", "mjpeg", "-q:v", "5", "-r", "10", "pipe:1"};

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
        if (vDev.isEmpty() || vDev.startsWith('<') || aDev.isEmpty() || aDev.startsWith('<')) {
            QMessageBox::critical(this, "Missing devices", "Please select valid video and audio devices.");
            return;
        }

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
        connect(recordProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int, QProcess::ExitStatus) {
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
        args += {currentOutputFile, "-map", "0:v", "-vf", "scale=720:576", "-f", "mjpeg", "-q:v", "5", "-r", "10", "pipe:1"};

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
            recordProc->write("q");
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

    QString buildDvSessionName() const {
        const QString tape = sanitizeTapeName(tapeNameEdit ? tapeNameEdit->text() : QString());
        return tape.isEmpty() ? QString() : ("dvgrab-" + tape);
    }

    QString activeDvSessionName() const {
        return currentDvSessionName.isEmpty() ? buildDvSessionName() : currentDvSessionName;
    }

    QString buildDvInnerCommand() const {
        const QString tape = sanitizeTapeName(tapeNameEdit->text());
        const QString base = remoteBaseEdit->text().trimmed();
        if (tape.isEmpty() || base.isEmpty()) return {};
        const QString dir = base + "/" + tape;
        const QString prefix = dir + "/" + tape + "-";
        return "mkdir -p " + shellSingleQuote(dir) + " && dvgrab -a -t --rewind --showstatus " + shellSingleQuote(prefix);
    }

    QString buildDvLaunchCommand() const {
        const QString session = buildDvSessionName();
        const QString inner = buildDvInnerCommand();
        if (session.isEmpty() || inner.isEmpty()) return {};
        const QString sessionQ = shellSingleQuote(session);
        const QString innerQ = shellSingleQuote(inner);
        const QString logQ = shellSingleQuote("/tmp/" + session + ".log");
        return "if command -v byobu >/dev/null 2>&1; then "
               "byobu new-session -d -s " + sessionQ + " -x 220 -y 40 sh -lc " + innerQ +
               "; elif command -v tmux >/dev/null 2>&1; then "
               "tmux new-session -d -s " + sessionQ + " -x 220 -y 40 sh -lc " + innerQ +
               "; else nohup sh -lc " + innerQ + " >" + logQ + " 2>&1 < /dev/null & fi";
    }

    QString buildDvMonitorCommand() const {
        const QString session = activeDvSessionName();
        if (session.isEmpty()) return {};
        const QString sessionQ = shellSingleQuote(session);
        const QString logQ = shellSingleQuote("/tmp/" + session + ".log");
        return "if command -v byobu >/dev/null 2>&1 && byobu has-session -t " + sessionQ + " 2>/dev/null; then "
               "byobu capture-pane -pJ -S -30 -t " + sessionQ +
               "; elif command -v tmux >/dev/null 2>&1 && tmux has-session -t " + sessionQ + " 2>/dev/null; then "
               "tmux capture-pane -pJ -S -30 -t " + sessionQ +
               "; elif test -f " + logQ + "; then tail -n 30 " + logQ +
               "; else echo '[session not found]'; exit 1; fi";
    }

    QString buildDvStopCommand() const {
        const QString session = activeDvSessionName();
        if (session.isEmpty()) return {};
        const QString sessionQ = shellSingleQuote(session);
        const QString pattQ = shellSingleQuote("dvgrab -a -t --rewind --showstatus");
        return "if command -v byobu >/dev/null 2>&1 && byobu has-session -t " + sessionQ + " 2>/dev/null; then "
               "byobu kill-session -t " + sessionQ +
               "; elif command -v tmux >/dev/null 2>&1 && tmux has-session -t " + sessionQ + " 2>/dev/null; then "
               "tmux kill-session -t " + sessionQ +
               "; else pkill -f " + pattQ + "; fi";
    }

    QString buildDvShutdownCommand() const {
        const QString inner =
            "sleep 1; if command -v systemctl >/dev/null 2>&1; then "
            "sudo -n systemctl poweroff || systemctl poweroff || sudo -n shutdown -h now || shutdown -h now; "
            "else sudo -n shutdown -h now || shutdown -h now; fi";
        return "nohup sh -lc " + shellSingleQuote(inner) + " >/tmp/remote-poweroff.log 2>&1 < /dev/null &";
    }

    void updateDvgrabPreview() {
        const QString tape = sanitizeTapeName(tapeNameEdit->text());
        const QString base = remoteBaseEdit->text().trimmed();
        const QString target = sshTargetEdit->text().trimmed();
        const QString session = buildDvSessionName();

        if (tape.isEmpty() || base.isEmpty()) {
            dvRemoteInfoLabel->setText("Destination: -");
            dvCmdPreview->setText("Enter a tape name to generate the detached dvgrab command.");
            dvStartBtn->setEnabled(false);
            dvAttachBtn->setEnabled(false);
            dvShutdownBtn->setEnabled(!target.isEmpty());
            return;
        }

        const QString dir = base + "/" + tape;
        dvRemoteInfoLabel->setText("Destination: " + dir + "    |    Session: " + session);
        const QString command = buildDvLaunchCommand();
        dvCmdPreview->setText(target.isEmpty() ? command : ("ssh " + target + " " + command));
        dvStartBtn->setEnabled(!target.isEmpty() && dvLaunchProc == nullptr);
        dvAttachBtn->setEnabled(!target.isEmpty());
        dvShutdownBtn->setEnabled(!target.isEmpty());
    }

    void startDvgrab() {
        const QString target = sshTargetEdit->text().trimmed();
        const QString tape = sanitizeTapeName(tapeNameEdit->text());
        const QString cmd = buildDvLaunchCommand();
        if (target.isEmpty()) {
            QMessageBox::warning(this, "SSH Target", "Please enter an SSH target, for example uni@recorder.");
            return;
        }
        if (tape.isEmpty()) {
            QMessageBox::warning(this, "Tape Name", "Please enter a tape name.");
            return;
        }
        if (cmd.isEmpty() || dvLaunchProc) return;

        currentDvSessionName = buildDvSessionName();
        lastDvStatusKey.clear();
        lastDvFile.clear();
        recentDvEvents.clear();
        dvEventLogEdit->clear();
        dvLiveLabel->setText("Waiting for detached session output...");
        appendDvEvent("Starting detached session " + currentDvSessionName + ".");
        dvStatusLabel->setText("Launching detached DV session on remote recorder...");

        dvLaunchProc = new QProcess(this);
        hideChildConsole(*dvLaunchProc);
        connect(dvLaunchProc, &QProcess::readyReadStandardOutput, this, [this] {
            appendLog(QString::fromLocal8Bit(dvLaunchProc->readAllStandardOutput()));
        });
        connect(dvLaunchProc, &QProcess::readyReadStandardError, this, [this] {
            appendLog(QString::fromLocal8Bit(dvLaunchProc->readAllStandardError()));
        });
        connect(dvLaunchProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus status) {
            const bool ok = (status == QProcess::NormalExit && code == 0);
            if (ok) {
                dvStatusLabel->setText("Detached DV session is running. Live monitor attached.");
                dvStopBtn->setEnabled(true);
                startDvgrabMonitor();
            } else {
                dvStatusLabel->setText(QString("Failed to launch detached DV session (exit code %1).").arg(code));
                appendDvEvent("Launch failed.");
                dvStopBtn->setEnabled(false);
            }
            dvLaunchProc->deleteLater();
            dvLaunchProc = nullptr;
            updateDvgrabPreview();
        });

        dvLaunchProc->start("ssh", {target, cmd});
        if (!dvLaunchProc->waitForStarted(5000)) {
            dvStatusLabel->setText("Failed to start SSH: " + dvLaunchProc->errorString());
            appendDvEvent("SSH launch failed.");
            dvLaunchProc->deleteLater();
            dvLaunchProc = nullptr;
            updateDvgrabPreview();
            return;
        }
        updateDvgrabPreview();
    }

    void startDvgrabMonitor() {
        if (activeDvSessionName().isEmpty()) {
            currentDvSessionName = buildDvSessionName();
        }
        if (activeDvSessionName().isEmpty()) {
            QMessageBox::warning(this, "Session", "Please enter a tape name first.");
            return;
        }
        appendDvEvent("Monitoring session " + activeDvSessionName() + ".");
        dvStatusLabel->setText("Monitoring detached DV session...");
        dvMonitorTimer->start();
        dvDetachBtn->setEnabled(true);
        refreshDvgrabMonitor();
    }

    std::optional<DvStatus> parseDvStatus(const QString &text) const {
        static const QRegularExpression re(
            QStringLiteral("^\"(?<file>.+)\":\\s+(?<mib>[0-9.]+)\\s+MiB\\s+(?<frames>\\d+)\\s+frames.*timecode\\s+(?<tc>[0-9:.]+)(?:\\s+date\\s+(?<date>.+))?$")
        );

        DvStatus best;
        for (const QString &raw : text.split('\n')) {
            const QString line = raw.trimmed();
            auto m = re.match(line);
            if (!m.hasMatch()) continue;
            DvStatus s;
            s.file = m.captured("file");
            s.mib = m.captured("mib").toDouble();
            s.frames = m.captured("frames").toLongLong();
            s.timecode = m.captured("tc");
            s.recordedAt = m.captured("date").trimmed();
            s.valid = true;
            best = s;
        }
        if (!best.valid) return std::nullopt;
        return best;
    }

    void appendDvEvent(const QString &event) {
        const QString stamped = QTime::currentTime().toString("HH:mm:ss") + "  " + event;
        if (!recentDvEvents.isEmpty() && recentDvEvents.back() == stamped) return;
        recentDvEvents << stamped;
        while (recentDvEvents.size() > 200) recentDvEvents.removeFirst();
        dvEventLogEdit->setPlainText(recentDvEvents.join('\n'));
        auto *bar = dvEventLogEdit->verticalScrollBar();
        bar->setValue(bar->maximum());
    }

    void updateDvMonitorUi(const QString &paneText) {
        if (dvVerboseCheck->isChecked()) {
            dvVerboseEdit->setPlainText(paneText.trimmed());
            auto *bar = dvVerboseEdit->verticalScrollBar();
            bar->setValue(bar->maximum());
        }

        const QStringList lines = paneText.split('\n');
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty()) continue;
            const QString lower = line.toLower();
            if (lower.contains("error") || lower.contains("warn") || lower.contains("drop") ||
                lower.contains("discontinuity") || lower.contains("signal lost") || lower.contains("aborted")) {
                appendDvEvent(line);
            }
        }

        auto status = parseDvStatus(paneText);
        if (!status) {
            return;
        }

        const DvStatus &s = *status;
        const QString key = s.file + "|" + s.timecode + "|" + QString::number(s.frames / 25);
        dvLiveLabel->setText(
            "File: " + QFileInfo(s.file).fileName() + "\n"
            "Full Path: " + s.file + "\n"
            "Size: " + QString::number(s.mib, 'f', 2) + " MiB\n"
            "Frames: " + QString::number(s.frames) + "\n"
            "Timecode: " + s.timecode +
            (s.recordedAt.isEmpty() ? QString() : ("\nRecorded At: " + s.recordedAt))
        );

        if (lastDvFile != s.file) {
            appendDvEvent("New segment: " + s.file);
            lastDvFile = s.file;
        }
        if (!lastDvStatusKey.isEmpty() && lastDvStatusKey != key && s.timecode != "") {
            // Keep live status fresh without logging every frame.
        }
        lastDvStatusKey = key;
    }

    void refreshDvgrabMonitor() {
        if (dvMonitorProc) return;
        const QString target = sshTargetEdit->text().trimmed();
        const QString cmd = buildDvMonitorCommand();
        if (target.isEmpty() || cmd.isEmpty()) return;

        dvMonitorProc = new QProcess(this);
        hideChildConsole(*dvMonitorProc);
        connect(dvMonitorProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus status) {
            QString out = QString::fromLocal8Bit(dvMonitorProc->readAllStandardOutput());
            QString err = QString::fromLocal8Bit(dvMonitorProc->readAllStandardError());
            if (!err.trimmed().isEmpty()) {
                if (!out.isEmpty() && !out.endsWith(QChar('\n'))) out += QChar('\n');
                out += err;
            }
            if (status == QProcess::NormalExit && code == 0) {
                updateDvMonitorUi(out);
            } else {
                if (dvMonitorTimer->isActive()) dvMonitorTimer->stop();
                dvStatusLabel->setText("DV monitor lost the remote session.");
                appendDvEvent("Monitor lost session.");
            }
            dvMonitorProc->deleteLater();
            dvMonitorProc = nullptr;
        });

        dvMonitorProc->start("ssh", {target, cmd});
        if (!dvMonitorProc->waitForStarted(5000)) {
            dvStatusLabel->setText("Failed to start monitor SSH: " + dvMonitorProc->errorString());
            appendDvEvent("Monitor SSH start failed.");
            dvMonitorProc->deleteLater();
            dvMonitorProc = nullptr;
        }
    }

    void stopDvgrabMonitor() {
        if (dvMonitorTimer) dvMonitorTimer->stop();
        if (dvMonitorProc) {
            dvMonitorProc->kill();
            dvMonitorProc->waitForFinished(1000);
            dvMonitorProc->deleteLater();
            dvMonitorProc = nullptr;
        }
        if (dvDetachBtn) dvDetachBtn->setEnabled(false);
    }

    void stopDvgrabSession() {
        const QString target = sshTargetEdit->text().trimmed();
        const QString cmd = buildDvStopCommand();
        if (target.isEmpty() || cmd.isEmpty()) return;

        stopDvgrabMonitor();
        appendDvEvent("Stopping session " + activeDvSessionName() + ".");
        QProcess p;
        hideChildConsole(p);
        p.start("ssh", {target, cmd});
        p.waitForFinished(10000);
        appendLog(QString::fromLocal8Bit(p.readAllStandardOutput()));
        appendLog(QString::fromLocal8Bit(p.readAllStandardError()));
        dvStatusLabel->setText("Detached DV session stop requested.");
        dvLiveLabel->setText("Session stopped.");
        dvStopBtn->setEnabled(false);
        currentDvSessionName.clear();
        lastDvStatusKey.clear();
        updateDvgrabPreview();
    }

    void shutdownRemoteMachine() {
        const QString target = sshTargetEdit->text().trimmed();
        if (target.isEmpty()) {
            QMessageBox::warning(this, "SSH Target", "Please enter an SSH target first.");
            return;
        }

        const auto answer = QMessageBox::warning(
            this,
            "Shut Down Remote Machine",
            "This will power off the remote recorder. Continue?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (answer != QMessageBox::Yes) return;

        stopDvgrabMonitor();
        appendDvEvent("Remote shutdown requested.");

        QProcess p;
        hideChildConsole(p);
        p.start("ssh", {target, buildDvShutdownCommand()});
        p.waitForFinished(10000);
        appendLog(QString::fromLocal8Bit(p.readAllStandardOutput()));
        appendLog(QString::fromLocal8Bit(p.readAllStandardError()));
        dvStatusLabel->setText("Remote shutdown command sent.");
        dvLiveLabel->setText("Remote machine is shutting down.");
        dvStopBtn->setEnabled(false);
        currentDvSessionName.clear();
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
        timerLabel->setText(hms(elapsed) + " elapsed - " + hms(remaining) + " remaining");
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
                previewLabel->setPixmap(px.scaled(previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
        }
    }

    void consumeStderr() {
        auto processLine = [this](const QString &lineRaw) {
            const QString line = lineRaw.trimmed();
            if (line.isEmpty()) return;

            static QRegularExpression progressRe(R"(^frame=\s*\d+)");
            static QRegularExpression timeRe(R"(time=(\d+):(\d+):(\d+))");
            const bool isProgressLine = progressRe.match(line).hasMatch();

            if (!isProgressLine) appendLog(line);
            if (isProgressLine) {
                auto m = timeRe.match(line);
                if (m.hasMatch()) {
                    qint64 h = m.captured(1).toLongLong();
                    qint64 min = m.captured(2).toLongLong();
                    qint64 sec = m.captured(3).toLongLong();
                    qint64 elapsed = h * 3600 + min * 60 + sec;
                    double pct = targetDurationSec > 0 ? std::min(1.0, double(elapsed) / double(targetDurationSec)) : 0.0;
                    progress->setValue(int(pct * 1000.0));
                    statusLabel->setText(QString("Recording %1 / %2").arg(hms(elapsed), hms(targetDurationSec)));
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
