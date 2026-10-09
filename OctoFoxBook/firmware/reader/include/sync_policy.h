#pragma once

#include <stdint.h>
#include <string.h>

// Shared by the worker, UI and no-board regression tests. No credentials here.
namespace ReaderSyncPolicy {
enum class Error : uint8_t {
    None, Busy, Memory, Configuration, Wifi, Clock, Authentication,
    Transport, Http, Protocol, Storage, Preparation
};

constexpr uint32_t kWifiAttemptMs = 20000;
constexpr bool canRequest(bool workerReady, bool running, bool exclusiveBusy) {
    return workerReady && !running && !exclusiveBusy;
}

inline bool isReadOnlyStatus(const char *command) {
    return strcmp(command, "PROVISION STATUS") == 0 ||
           strcmp(command, "WIFI FALLBACK STATUS") == 0 ||
             strcmp(command, "NETWORK STATUS") == 0 ||
             strcmp(command, "STORAGE STATUS") == 0 ||
           strcmp(command, "STORAGE RECOVERY STATUS") == 0;
}

inline Error connectionError(const char *reason) {
    if (strcmp(reason, "time-sync-timeout") == 0) return Error::Clock;
    if (strcmp(reason, "wifi-connect-timeout") == 0 ||
        strcmp(reason, "sync-paused") == 0) return Error::Wifi;
    return Error::Configuration;
}

constexpr Error httpError(int code) {
    return code == 401 || code == 403 ? Error::Authentication
         : code <= 0 ? Error::Transport : Error::Http;
}

inline Error downloadError(const char *reason, int code) {
    if (code != 0 && code != 200) return httpError(code);
    if (strcmp(reason, "wifi-connect-timeout") == 0 ||
        strcmp(reason, "time-sync-timeout") == 0 || strcmp(reason, "sync-paused") == 0)
        return connectionError(reason);
    if (strcmp(reason, "transfer-allocation-failed") == 0) return Error::Memory;
    if (strcmp(reason, "tls-or-network-failed") == 0 || strcmp(reason, "http-begin-failed") == 0 ||
        strcmp(reason, "transfer-timeout") == 0 || strcmp(reason, "stream-failed") == 0 ||
        strcmp(reason, "incomplete-response") == 0) return Error::Transport;
    if (strcmp(reason, "invalid-content-type") == 0 || strcmp(reason, "invalid-size") == 0 ||
        strcmp(reason, "invalid-fb2-body") == 0 || strcmp(reason, "response-too-large") == 0)
        return Error::Protocol;
    if (strcmp(reason, "mkdir-failed") == 0 || strcmp(reason, "path-failed") == 0 ||
        strcmp(reason, "open-part-failed") == 0 || strcmp(reason, "sd-write-failed") == 0 ||
        strcmp(reason, "sd-verification-failed") == 0 || strcmp(reason, "cache-invalidate-failed") == 0 ||
        strcmp(reason, "publish-failed") == 0) return Error::Storage;
    return Error::Configuration;
}

inline const char *errorCode(Error error) {
    switch (error) {
        case Error::None: return "none";
        case Error::Busy: return "storage-busy";
        case Error::Memory: return "allocation-failed";
        case Error::Configuration: return "profile-invalid";
        case Error::Wifi: return "wifi-connect-failed";
        case Error::Clock: return "time-sync-failed";
        case Error::Authentication: return "account-rejected";
        case Error::Transport: return "tls-or-network-failed";
        case Error::Http: return "http-status";
        case Error::Protocol: return "invalid-sync-response";
        case Error::Storage: return "sd-write-or-verification-failed";
        case Error::Preparation: return "book-preparation-failed";
    }
    return "unknown";
}

inline const char *errorLabel(Error error) {
    switch (error) {
        case Error::Busy: return "ЗАНЯТО · ПОВТОРИТЕ ПОЗЖЕ";
        case Error::Memory: return "НЕ ХВАТАЕТ ПАМЯТИ";
        case Error::Configuration: return "НУЖНА НАСТРОЙКА ПОДКЛЮЧЕНИЯ";
        case Error::Wifi: return "НЕТ ПОДКЛЮЧЕНИЯ К WI-FI";
        case Error::Clock: return "НЕ УДАЛОСЬ ОБНОВИТЬ ВРЕМЯ";
        case Error::Authentication: return "ПРОВЕРЬТЕ ЛОГИН И ПАРОЛЬ";
        case Error::Transport: return "ОШИБКА HTTPS ИЛИ СЕТИ";
        case Error::Http: return "СЕРВЕР ВЕРНУЛ ОШИБКУ";
        case Error::Protocol: return "НЕВЕРНЫЙ ОТВЕТ СЕРВЕРА";
        case Error::Storage: return "ОШИБКА ЗАПИСИ НА SD";
        case Error::Preparation: return "НЕ УДАЛОСЬ ПОДГОТОВИТЬ КНИГУ";
        default: return "НЕ УДАЛОСЬ · ПОВТОРИТЕ";
    }
}
}
