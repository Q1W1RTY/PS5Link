// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <QWindow>
#include <QPointer>
#include <memory>

class QKeyEvent;
class QExposeEvent;
class QResizeEvent;
class QMouseEvent;
class QEvent;
class QTimer;
class QmlSettings;
class StreamSession;
struct AVFrame;
struct AVBufferRef;

class Dx12ReplayWindow final : public QWindow
{
    Q_OBJECT
public:
    Dx12ReplayWindow();
    ~Dx12ReplayWindow() override;

    AVBufferRef *decoderDevice() const;
    bool queueVideo(const AVFrame &frame, bool continuous = true);
    void prepareVideo(QSize input, bool fullscreen);
    bool isPreparingVideo() const;
    void report();
    unsigned presentedFrames() const;
    unsigned generatedFrames() const;
    void bindSession(StreamSession *session, QmlSettings *settings, int quality);
    void setVsrQuality(int quality);
    void setFrameGeneration(int mode);
    void applyWindowMode(int mode);
    unsigned dlssGeneratedFrames() const;
    unsigned dlssSrFrames() const;

public slots:
    void openSettings();

signals:
    void gameplayKey(QKeyEvent *event);
    void statsVisibilityChanged(bool visible);
    void vsrQualityChanged(int quality);
    void disconnectRequested(bool sleep, bool exitApp);
    void allSettingsRequested();
    void videoQueued();
    void firstFramePresented();
    void renderingFailed(const QString &error);
    void overlayAction(int action, int value);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void exposeEvent(QExposeEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    bool event(QEvent *ev) override;

private:
    void feedMouse(QMouseEvent *event, int buttonState);
    void refreshOverlay();
    void updateCursorVisibility(bool userMovedMouse);
    struct Pipeline;
    std::unique_ptr<Pipeline> pipeline;
    struct RenderWorker;
    std::unique_ptr<RenderWorker> worker;
    void updateViewport();
    QPointer<StreamSession> session;
    QPointer<QmlSettings> settings;
    QTimer *cursorTimer = nullptr;
    bool cursorHidden = false;
    bool touchpadMouseActive = false;
    bool touchpadMouseClickBtn = false;
};
