#pragma once

#include <QSettings>
#include <QVariant>

struct Ps5LinkBoostOptions
{
    bool wifiMediaMode = true;
    bool preventSleep = true;
    bool turboMode = true;
};

#ifdef PS5LINK_BUILD
class QGuiApplication;
bool Ps5LinkOnline();
bool Ps5LinkIsSecretProtected(const QVariant &value);
bool Ps5LinkSetSecret(QSettings &settings, const QString &key, const QVariant &value);
QVariant Ps5LinkGetSecret(const QSettings &settings, const QString &key, QMetaType type);
int Ps5LinkImport(const QString &sourceName);
int Ps5LinkReplay(QGuiApplication &app, const QString &fileName, bool fullscreen, bool directX12);
void Ps5LinkBeginStreamBoost(const Ps5LinkBoostOptions &options = {});
void Ps5LinkEndStreamBoost();
int Ps5LinkGetSystemVolume();
bool Ps5LinkSetSystemVolume(int percent);
#else
inline bool Ps5LinkOnline() { return true; }
inline void Ps5LinkBeginStreamBoost(const Ps5LinkBoostOptions & = {}) {}
inline void Ps5LinkEndStreamBoost() {}
inline int Ps5LinkGetSystemVolume() { return -1; }
inline bool Ps5LinkSetSystemVolume(int) { return false; }
inline bool Ps5LinkIsSecretProtected(const QVariant &) { return false; }
inline bool Ps5LinkSetSecret(QSettings &settings, const QString &key, const QVariant &value)
{
    settings.setValue(key, value);
    return true;
}
inline QVariant Ps5LinkGetSecret(const QSettings &settings, const QString &key, QMetaType)
{
    return settings.value(key);
}
#endif
