/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Edge. All rights reserved.                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_USER_SIGNIN_UTIL_H_
#define XENIA_KERNEL_XAM_USER_SIGNIN_UTIL_H_

#include <cstdint>

#include "xenia/kernel/xam/xam.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// Deterministic Xbox Live online XUID (0x0009............) derived from an
// offline profile XUID when live_signin is enabled in memory without rewriting
// the profile's Account file on disk.
inline constexpr uint64_t DeriveOfflineLiveXuid(uint64_t offline_xuid) {
  return (0x9ULL << 48) | (offline_xuid & 0x7FFFFFFFULL);
}

inline bool ResolveAccountLiveEnabled(const X_XAMACCOUNTINFO& account,
                                      bool live_signin_override) {
  return account.IsLiveEnabled() || live_signin_override;
}

inline uint64_t ResolveAccountOnlineXuid(uint64_t offline_xuid,
                                         const X_XAMACCOUNTINFO& account,
                                         bool live_signin_override) {
  if (!ResolveAccountLiveEnabled(account, live_signin_override)) {
    return 0;
  }
  const uint64_t stored = account.GetOnlineXUID();
  return stored ? stored : DeriveOfflineLiveXuid(offline_xuid);
}

inline uint32_t ResolveAccountSigninState(const X_XAMACCOUNTINFO& account,
                                          bool live_signin_override) {
  // SignInState::SignedInToLive = 2, SignInState::SignedInLocally = 1.
  return ResolveAccountLiveEnabled(account, live_signin_override) ? 2u : 1u;
}

inline uint64_t ResolveAccountLogonXuid(uint64_t offline_xuid,
                                        const X_XAMACCOUNTINFO& account,
                                        bool live_signin_override) {
  return ResolveAccountLiveEnabled(account, live_signin_override)
             ? ResolveAccountOnlineXuid(offline_xuid, account,
                                        live_signin_override)
             : offline_xuid;
}

inline uint32_t ResolveAccountSubscriptionTier(const X_XAMACCOUNTINFO& account,
                                               bool live_signin_override) {
  const auto tier = account.GetSubscriptionTier();
  if (tier == X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierNone &&
      ResolveAccountLiveEnabled(account, live_signin_override)) {
    return static_cast<uint32_t>(
        X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierGold);
  }
  return static_cast<uint32_t>(tier);
}

inline uint32_t ResolveAccountCachedFlags(const X_XAMACCOUNTINFO& account,
                                          bool live_signin_override) {
  uint32_t flags = account.GetCachedFlags();
  if (ResolveAccountLiveEnabled(account, live_signin_override) &&
      account.GetSubscriptionTier() ==
          X_XAMACCOUNTINFO::AccountSubscriptionTier::kSubscriptionTierNone) {
    flags |= (static_cast<uint32_t>(
                  X_XAMACCOUNTINFO::AccountSubscriptionTier::
                      kSubscriptionTierGold)
              << 20) &
             X_XAMACCOUNTINFO::AccountUserFlags::kSubscriptionTierMask;
  }
  return flags;
}

inline uint32_t ResolveAccountReservedFlags(const X_XAMACCOUNTINFO& account,
                                            bool live_signin_override) {
  return account.GetReservedFlags() |
         (ResolveAccountLiveEnabled(account, live_signin_override)
              ? X_XAMACCOUNTINFO::AccountReservedFlags::kLiveEnabled
              : 0u);
}

inline uint32_t ResolveAccountUserFlags(const X_XAMACCOUNTINFO& account,
                                        bool live_signin_override) {
  return ResolveAccountCachedFlags(account, live_signin_override) |
         ResolveAccountReservedFlags(account, live_signin_override);
}

inline uint64_t SelectSigninInfoXuid(uint64_t offline_xuid,
                                     uint64_t online_xuid, uint64_t logon_xuid,
                                     uint32_t flags) {
  // ONLINE_XUID_ONLY wins when both are set.
  if (flags & X_USER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY) {
    return online_xuid;
  }
  if (flags & X_USER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY) {
    return offline_xuid;
  }
  return logon_xuid;
}

inline X_HRESULT SelectGetXuidResult(uint64_t offline_xuid,
                                     uint64_t online_xuid,
                                     bool signed_in_to_live, uint32_t type_mask,
                                     uint64_t* out_xuid) {
  // Only a user signed in to Live has an online XUID to give.
  uint32_t types = X_USER_XUID_OFFLINE;
  if (signed_in_to_live) {
    types |= X_USER_XUID_ONLINE;
  }
  types &= type_mask;

  if (types & X_USER_XUID_ONLINE) {
    *out_xuid = online_xuid;
    return X_E_SUCCESS;
  }
  if (types & X_USER_XUID_OFFLINE) {
    *out_xuid = offline_xuid;
    return X_E_SUCCESS;
  }
  return X_E_NO_SUCH_USER;
}

// With no Live service, a user signed in to Live holds every Live privilege.
inline constexpr bool IsLivePrivilegeGranted(uint32_t mask) {
  return mask >= 0xD1 && mask <= 0xFF;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_USER_SIGNIN_UTIL_H_
