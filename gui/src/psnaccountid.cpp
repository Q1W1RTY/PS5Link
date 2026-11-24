#include "psnaccountid.h"
#include "jsonrequester.h"
#include <QUrl>

#include <qjsonobject.h>
#include <QObject>
#include <QDebug>
PSNAccountID::PSNAccountID(Settings *settings, QObject *parent, bool saveSettings)
    : QObject(parent)
    , settings(settings)
    , saveSettings(saveSettings)
{
    basicAuthHeader = JsonRequester::generateBasicAuthHeader(PSNAuth::CLIENT_ID, PSNAuth::CLIENT_SECRET);
}

void PSNAccountID::GetPsnAccountId(QString redirectCode) {
    QString body = QString("grant_type=authorization_code&code=%1&scope=psn:clientapp referenceDataService:countryConfig.read pushNotification:webSocket.desktop.connect sessionManager:remotePlaySession.system.update&redirect_uri=https://remoteplay.dl.playstation.net/remoteplay/redirect&").arg(QString::fromLatin1(QUrl::toPercentEncoding(redirectCode)));
    JsonRequester* requester = new JsonRequester(this);
    connect(requester, &JsonRequester::requestFinished, this, &PSNAccountID::handleAccessTokenResponse);
    connect(requester, &JsonRequester::requestError, this, &PSNAccountID::handleErrorResponse);
    requester->makePostRequest(PSNAuth::TOKEN_URL, basicAuthHeader, "application/x-www-form-urlencoded", std::move(body));
}

void PSNAccountID::handleAccessTokenResponse(const QString& url, const QJsonDocument& jsonDocument) {
    QJsonObject object = jsonDocument.object();
    QString access_token = object.value("access_token").toString();
    QString refresh_token = object.value("refresh_token").toString();
    QDateTime currentTime = QDateTime::currentDateTime();
    auto secondsLeft = object.value("expires_in").toInt();
    QDateTime expiry = currentTime.addSecs(secondsLeft);
    QString access_token_expiry = expiry.toString(settings->GetTimeFormat());
    if (access_token.isEmpty() || refresh_token.isEmpty() || secondsLeft <= 0) {
        emit AccountIDError({}, tr("PSN did not return valid credentials."));
        emit Finished();
        return;
    }
    credentials.accessToken = access_token;
    credentials.refreshToken = std::move(refresh_token);
    credentials.expiry = std::move(access_token_expiry);

    QString accountInfoUrl = QString("%1/%2").arg(PSNAuth::TOKEN_URL).arg(access_token);
    JsonRequester* requester = new JsonRequester(this);
    connect(requester, &JsonRequester::requestFinished, this, &PSNAccountID::handUserIDResponse);
    connect(requester, &JsonRequester::requestError, this, &PSNAccountID::handleErrorResponse);
    requester->makeGetRequest(accountInfoUrl, basicAuthHeader, "application/json");
}

void PSNAccountID::handUserIDResponse(const QString& url, const QJsonDocument& jsonDocument) {
    QJsonObject object = jsonDocument.object();
    QString user_id = object.value("user_id").toString();
    bool valid = false;
    const quint64 account = user_id.toULongLong(&valid);
    if (!valid) {
        emit AccountIDError({}, tr("PSN did not return a valid account ID."));
        emit Finished();
        return;
    }
    QByteArray accountBytes(8, Qt::Uninitialized);
    for(int i = 0; i < 8; ++i)
        accountBytes[i] = static_cast<char>((account >> (i * 8)) & 0xff);
    credentials.accountId = QString::fromLatin1(accountBytes.toBase64());
    if (saveSettings) {
        settings->SetPsnAuthToken(credentials.accessToken);
        settings->SetPsnRefreshToken(credentials.refreshToken);
        settings->SetPsnAuthTokenExpiry(credentials.expiry);
        settings->SetPsnAccountId(credentials.accountId);
    }
    emit AccountIDResponse(credentials.accountId);
    emit Finished();
}

void PSNAccountID::handleErrorResponse(const QString& url, const QString& error, const QNetworkReply::NetworkError& err) {
    emit AccountIDError(url, error);
    emit Finished();
}
