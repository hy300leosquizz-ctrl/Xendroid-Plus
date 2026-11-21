// Offline Xbox Live sign-in semantics (xenia/kernel/xam/user_signin_util.h):
// local profiles stay SignedInLocally by default, while profiles with
// kLiveEnabled on disk or titles with live_signin = true report SignedInToLive,
// expose a deterministic 0x0009... online XUID while keeping the offline 0xE030...
// XUID intact for saves, combine cached + reserved user flags, and grant Live
// privileges 0xD1..0xFF without mutating the Account struct on disk.

#include <cstdint>
#include <cstdio>
#include <cstring>

#define FMT_HEADER_ONLY 1
#include "xenia/kernel/xam/user_signin_util.h"

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using namespace xe;
  using namespace xe::kernel::xam;

  constexpr uint64_t kOfflineXuid = 0xE0300000125014AFULL;
  X_XAMACCOUNTINFO account = {};
  const X_XAMACCOUNTINFO untouched = account;

  // 1. Default offline profile (live_signin = false): must remain SignedInLocally
  // so titles that try connecting to Xbox Live servers do not regress.
  expect(!ResolveAccountLiveEnabled(account, false),
         "offline profile without override is not Live-enabled");
  expect(ResolveAccountSigninState(account, false) == 1u,
         "offline profile reports SignedInLocally (1)");
  expect(ResolveAccountOnlineXuid(kOfflineXuid, account, false) == 0,
         "offline profile has no online XUID");
  expect(ResolveAccountLogonXuid(kOfflineXuid, account, false) == kOfflineXuid,
         "offline profile logon XUID is offline XUID");
  expect((ResolveAccountUserFlags(account, false) &
          X_XAMACCOUNTINFO::AccountReservedFlags::kLiveEnabled) == 0,
         "offline profile user flags omit kLiveEnabled");

  uint64_t xuid_out = 0xDEADBEEFULL;
  expect(SelectGetXuidResult(kOfflineXuid, 0, false, X_USER_XUID_ONLINE,
                             &xuid_out) == X_E_NO_SUCH_USER,
         "XamUserGetXUID(ONLINE) fails when SignedInLocally");
  expect(xuid_out == 0xDEADBEEFULL,
         "failed XamUserGetXUID(ONLINE) leaves output untouched");
  expect(SelectGetXuidResult(kOfflineXuid, 0, false,
                             X_USER_XUID_OFFLINE | X_USER_XUID_ONLINE,
                             &xuid_out) == X_E_SUCCESS &&
             xuid_out == kOfflineXuid,
         "XamUserGetXUID(OFFLINE|ONLINE) returns offline XUID when local");
  expect(SelectSigninInfoXuid(kOfflineXuid, 0, kOfflineXuid,
                              X_USER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY) == 0,
         "XamUserGetSigninInfo(ONLINE_ONLY) writes 0 XUID when local");
  expect(SelectSigninInfoXuid(kOfflineXuid, 0, kOfflineXuid, 0) == kOfflineXuid,
         "XamUserGetSigninInfo(0) writes offline logon XUID when local");

  // 2. Per-title live_signin = true override (e.g. Fight Night Champion 45410915):
  // reports SignedInToLive, deterministic online XUID, Gold tier, and kLiveEnabled
  // without mutating the Account structure.
  const uint64_t derived_online = DeriveOfflineLiveXuid(kOfflineXuid);
  expect((derived_online >> 48) == 0x0009ULL && derived_online != 0,
         "derived online XUID uses Xbox Live 0x0009 prefix");
  expect(DeriveOfflineLiveXuid(kOfflineXuid) == derived_online,
         "derived online XUID is deterministic across launches");
  expect(ResolveAccountLiveEnabled(account, true),
         "live_signin override enables Live in memory");
  expect(ResolveAccountSigninState(account, true) == 2u,
         "live_signin override reports SignedInToLive (2)");
  expect(ResolveAccountOnlineXuid(kOfflineXuid, account, true) ==
             derived_online,
         "live_signin override exposes derived online XUID");
  expect(ResolveAccountLogonXuid(kOfflineXuid, account, true) == derived_online,
         "live_signin override uses online XUID as logon XUID");
  expect(ResolveAccountSubscriptionTier(account, true) ==
             X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierGold,
         "live_signin override defaults unset tier to Gold (6)");

  const uint32_t live_flags = ResolveAccountUserFlags(account, true);
  expect((live_flags & X_XAMACCOUNTINFO::AccountReservedFlags::kLiveEnabled) !=
             0,
         "XamUserGetUserFlags includes kLiveEnabled reserved bit");
  expect(((live_flags &
           X_XAMACCOUNTINFO::AccountUserFlags::kSubscriptionTierMask) >>
          20) ==
             X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierGold,
         "XamUserGetUserFlags includes Gold subscription tier");
  expect(std::memcmp(&account, &untouched, sizeof(account)) == 0,
         "live_signin override never mutates on-disk Account bytes");

  expect(SelectSigninInfoXuid(kOfflineXuid, derived_online, derived_online, 0) ==
             derived_online,
         "XamUserGetSigninInfo(0) returns online logon XUID when on Live");
  expect(SelectSigninInfoXuid(kOfflineXuid, derived_online, derived_online,
                              X_USER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY) ==
             kOfflineXuid,
         "XamUserGetSigninInfo(OFFLINE_ONLY) still returns savegame XUID");
  expect(SelectSigninInfoXuid(
             kOfflineXuid, derived_online, derived_online,
             X_USER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY |
                 X_USER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY) == derived_online,
         "ONLINE_XUID_ONLY wins when both signin info flags are set");
  expect(SelectGetXuidResult(kOfflineXuid, derived_online, true,
                             X_USER_XUID_ONLINE, &xuid_out) == X_E_SUCCESS &&
             xuid_out == derived_online,
         "XamUserGetXUID(ONLINE) returns online XUID when SignedInToLive");
  expect(SelectGetXuidResult(kOfflineXuid, derived_online, true,
                             X_USER_XUID_OFFLINE,
                             &xuid_out) == X_E_SUCCESS &&
             xuid_out == kOfflineXuid,
         "XamUserGetXUID(OFFLINE) preserves offline save XUID when on Live");

  expect(IsLivePrivilegeGranted(0xD1) && IsLivePrivilegeGranted(0xFE) &&
             IsLivePrivilegeGranted(0xFF),
         "Live privileges 0xD1..0xFF are granted");
  expect(!IsLivePrivilegeGranted(0x00) && !IsLivePrivilegeGranted(0xD0) &&
             !IsLivePrivilegeGranted(0x100),
         "masks outside 0xD1..0xFF are not granted");

  // 3. Profile explicitly marked Live-enabled on disk keeps its own xuid_online
  // and subscription tier even when live_signin = false.
  X_XAMACCOUNTINFO disk_live = {};
  constexpr uint64_t kStoredOnline = 0x0009000076543210ULL;
  disk_live.ToggleLiveFlag(true);
  disk_live.xuid_online = kStoredOnline;
  disk_live.SetSubscriptionTier(
      X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierSilver);
  expect(ResolveAccountSigninState(disk_live, false) == 2u,
         "disk Live-enabled profile signs in to Live without cvar");
  expect(ResolveAccountOnlineXuid(kOfflineXuid, disk_live, false) ==
             kStoredOnline,
         "disk Live-enabled profile keeps its stored online XUID");
  expect(ResolveAccountSubscriptionTier(disk_live, false) ==
             X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierSilver,
         "disk Live-enabled profile keeps its explicit subscription tier");

  if (failures == 0) { std::printf("xam_live_signin_test: ok\n"); }
  return failures == 0 ? 0 : 1;
}
