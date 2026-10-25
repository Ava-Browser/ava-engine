/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDatabase/Database.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/SessionStore.h>
#include <LibWebView/StorageJar.h>

namespace WebView {

BrowsingSession::BrowsingSession(IsPrivate is_private, Optional<ByteString> container_name)
    : m_is_private(is_private)
    , m_container_name(move(container_name))
{
}

BrowsingSession::~BrowsingSession() = default;

NonnullRefPtr<BrowsingSession> BrowsingSession::create(IsPrivate is_private)
{
    auto session = adopt_ref(*new BrowsingSession(is_private));

    if (is_private == IsPrivate::Yes) {
        session->cookie_jar = CookieJar::create(IsPrivate::Yes);
        session->storage_jar = StorageJar::create();
        session->blob_url_store = make<BlobURLStore>();
        session->hsts_store = HSTSStore::create();
        session->favicon_store = FaviconStore::create();
        session->history_store = HistoryStore::create_disabled();
        session->session_store = SessionStore::create();
    }

    return session;
}

ErrorOr<NonnullRefPtr<BrowsingSession>> BrowsingSession::create_container(ByteString name, Database::Database& database)
{
    auto session = adopt_ref(*new BrowsingSession(IsPrivate::No, name));

    session->cookie_jar = TRY(CookieJar::create(database));
    session->storage_jar = TRY(StorageJar::create(database));
    session->blob_url_store = make<BlobURLStore>();
    session->hsts_store = HSTSStore::create();
    session->favicon_store = FaviconStore::create();
    session->history_store = HistoryStore::create_disabled();
    session->session_store = SessionStore::create();

    return session;
}

}
