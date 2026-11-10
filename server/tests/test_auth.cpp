#include <cstdio>
#include <ctime>
#include <string>

#include "api/auth.h"
#include "db/db.h"
#include "db/sessions.h"
#include "db/users.h"
#include "httplib.h"
#include "util/crypto.h"

static int g_fail = 0;

static void expect(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++g_fail;
    }
}

int main() {
    std::string err;
    expect(db::open(":memory:", err), "open memory db");
    expect(db::init_schema(err), "init schema");

    long long uid = 0;
    expect(db::create_user("alice", "salt", "hash", 5368709120LL, uid, err), "create user");

    const std::string token = crypto::random_hex(32);
    expect(token.size() == 64, "token is 64 hex chars");

    const long long now = static_cast<long long>(std::time(nullptr));
    expect(db::create_session(uid, crypto::sha256_hex(token), now + 3600, err), "create session");

    httplib::Request req;
    req.set_header("Authorization", "Bearer " + token);
    long long got = 0;
    expect(auth::resolve(req, got), "valid token resolves");
    expect(got == uid, "resolved user_id");

    httplib::Request no_header;
    long long ignored = 0;
    expect(!auth::resolve(no_header, ignored), "missing header rejected");

    httplib::Request forged;
    forged.set_header("Authorization", "Bearer " + std::string(64, 'f'));
    expect(!auth::resolve(forged, ignored), "forged token rejected");

    const std::string expired_token = crypto::random_hex(32);
    db::create_session(uid, crypto::sha256_hex(expired_token), now - 1, err);
    httplib::Request expired;
    expired.set_header("Authorization", "Bearer " + expired_token);
    expect(!auth::resolve(expired, ignored), "expired token rejected");

    db::delete_session(crypto::sha256_hex(token), err);
    expect(!auth::resolve(req, ignored), "token rejected after logout");

    db::close();
    std::printf("auth: %s (%d failed)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
