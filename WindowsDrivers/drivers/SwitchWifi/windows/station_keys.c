/* SPDX-License-Identifier: GPL-2.0-only */
#include "station_keys.h"
#include "join_firmware.h"

typedef struct KEY_CHANGE {
    UCHAR Slot, Id;
    const UCHAR *Key, *Pn;
    BOOLEAN Skip;
} KEY_CHANGE;

BOOLEAN SwifiKeysHaveGroup(const SWIFI_STATION_KEYS *Keys)
{
    for (ULONG I = 0; I < 4; ++I)
        if (Keys->Slot[I].Present) return TRUE;
    return FALSE;
}

static NTSTATUS Identify(WDI_CIPHER_KEY_TYPE_STRUCT Type, const WDI_MAC_ADDRESS *Address,
    const UCHAR Peer[6], BOOLEAN HasId, ULONG Id, KEY_CHANGE *Change)
{
    if (Type.CipherAlgorithm != WDI_CIPHER_ALGO_CCMP || Type.Static ||
        (Type.KeyType != WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY && Type.KeyType != WDI_CIPHER_KEY_TYPE_GROUP_KEY))
        return STATUS_NOT_SUPPORTED;
    if (HasId && Id > 3) return STATUS_INVALID_PARAMETER;
    static const UCHAR Broadcast[6] = {255,255,255,255,255,255};
    if (Address && !RtlEqualMemory(Address->Address, Peer, 6) &&
        !(Type.KeyType == WDI_CIPHER_KEY_TYPE_GROUP_KEY && RtlEqualMemory(Address->Address, Broadcast, 6)))
        return STATUS_INVALID_PARAMETER;
    if (Type.KeyType == WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY) {
        if (!Address || Type.Direction != WDI_CIPHER_KEY_DIRECTION_BOTH) return STATUS_NOT_SUPPORTED;
        Change->Slot = 4;
    } else {
        if (!HasId || (Type.Direction != WDI_CIPHER_KEY_DIRECTION_INBOUND &&
            Type.Direction != WDI_CIPHER_KEY_DIRECTION_BOTH)) return STATUS_NOT_SUPPORTED;
        Change->Slot = (UCHAR)Id;
    }
    Change->Id = HasId ? (UCHAR)Id : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS Apply(SWIFI_STATION_KEYS *Keys, SWIFI_DEVICE *D, const UCHAR Peer[6],
    KEY_CHANGE *Changes, ULONG Count, BOOLEAN Add, BOOLEAN *Uncertain)
{
    ULONG Seen = 0;
    for (ULONG I = 0; I < Count; ++I) {
        KEY_CHANGE *C = &Changes[I];
        SWIFI_KEY_SLOT *Slot = &Keys->Slot[C->Slot];
        if (Seen & (1u << C->Slot)) return STATUS_INVALID_PARAMETER;
        Seen |= 1u << C->Slot;
        if (!Add) {
            if (!Slot->Present) C->Skip = TRUE;
            else if (Slot->Id != C->Id) return STATUS_INVALID_PARAMETER;
            continue;
        }
        for (ULONG J = 0; J < RTL_NUMBER_OF(Keys->Slot); ++J) {
            SWIFI_KEY_SLOT *Old = &Keys->Slot[J];
            if (!Old->Known || !RtlEqualMemory(Old->Key, C->Key, 16)) continue;
            /* Reinstalling the same TK resets hardware packet numbers. Preserve
             * live counters on an idempotent retry, reject an unsafe rebind. */
            if (!swifi_keep_key_on_retry(J == C->Slot && Old->Id == C->Id, Old->Present, C->Pn, Old->Pn))
                return STATUS_INVALID_PARAMETER;
            C->Skip = TRUE;
        }
        for (ULONG J = 0; J < I; ++J)
            if (RtlEqualMemory(C->Key, Changes[J].Key, 16)) return STATUS_INVALID_PARAMETER;
        if (Slot->Present && Slot->Id != C->Id) return STATUS_NOT_SUPPORTED;
    }
    NTSTATUS Status = SwifiDataDrainEapol(D);
    if (!NT_SUCCESS(Status)) return Status;
    for (ULONG I = 0; I < Count; ++I) {
        KEY_CHANGE *C = &Changes[I];
        if (C->Skip) continue;
        Status = SwifiJoinKey(&D->Hw.Control, C->Id, C->Slot == 4 ? Peer : NULL, C->Key, C->Pn);
        if (!NT_SUCCESS(Status)) { *Uncertain = TRUE; return Status; }
        SWIFI_KEY_SLOT *Slot = &Keys->Slot[C->Slot];
        Slot->Present = Add;
        if (Add) {
            Slot->Known = TRUE;
            Slot->Id = C->Id;
            RtlCopyMemory(Slot->Key, C->Key, 16);
            RtlCopyMemory(Slot->Pn, C->Pn, 6);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwifiKeysChange(SWIFI_STATION_KEYS *Keys, SWIFI_DEVICE *D, const UCHAR Peer[6],
    TLV_CONTEXT *Tlv, WDI_MESSAGE_HEADER *H, ULONG Bytes, BOOLEAN Add, BOOLEAN *Uncertain)
{
    KEY_CHANGE Changes[5] = {0};
    NTSTATUS Status;
    ULONG Count;
    *Uncertain = FALSE;
    if (Add) {
        WDI_SET_ADD_CIPHER_KEYS_PARAMETERS P = {0};
        Status = (NTSTATUS)ParseWdiSetAddCipherKeysToIhv(Bytes, (PUCHAR)(H+1), Tlv, &P);
        Count = P.SetCipherKey.ElementCount;
        if (NT_SUCCESS(Status) && (!Count || Count > RTL_NUMBER_OF(Changes))) Status = STATUS_INVALID_PARAMETER;
        for (ULONG I = 0; NT_SUCCESS(Status) && I < Count; ++I) {
            WDI_SET_ADD_CIPHER_KEYS_CONTAINER *K = &P.SetCipherKey.pElements[I];
            if (!K->Optional.CCMPKey_IsPresent || K->CCMPKey.ElementCount != 16 ||
                !K->Optional.ReceiveSequenceCount_IsPresent || K->Optional.LinkId_IsPresent ||
                K->Optional.TKIPInfo_IsPresent || K->Optional.GCMPKey_IsPresent || K->Optional.WEPKey_IsPresent ||
                K->Optional.BIPKey_IsPresent || K->Optional.IHVKey_IsPresent || K->Optional.GCMP_256Key_IsPresent ||
                K->Optional.BIP_GMAC_256Key_IsPresent) { Status = STATUS_NOT_SUPPORTED; break; }
            Status = Identify(K->CipherKeyTypeInfo, K->Optional.PeerMacAddress_IsPresent ? &K->PeerMacAddress : NULL,
                Peer, K->Optional.CipherKeyID_IsPresent, K->CipherKeyID.CipherKeyID, &Changes[I]);
            Changes[I].Key = K->CCMPKey.pElements;
            Changes[I].Pn = K->ReceiveSequenceCount.ReceiveSequenceCount;
        }
        if (NT_SUCCESS(Status)) Status = Apply(Keys, D, Peer, Changes, Count, TRUE, Uncertain);
        CleanupParsedWdiSetAddCipherKeysToIhv(&P);
    } else {
        WDI_SET_DELETE_CIPHER_KEYS_PARAMETERS P = {0};
        Status = (NTSTATUS)ParseWdiSetDeleteCipherKeysToIhv(Bytes, (PUCHAR)(H+1), Tlv, &P);
        Count = P.CipherKeyInfo.ElementCount;
        if (NT_SUCCESS(Status) && (!Count || Count > RTL_NUMBER_OF(Changes))) Status = STATUS_INVALID_PARAMETER;
        for (ULONG I = 0; NT_SUCCESS(Status) && I < Count; ++I) {
            WDI_SET_DELETE_CIPHER_KEYS_CONTAINER *K = &P.CipherKeyInfo.pElements[I];
            if (K->Optional.LinkId_IsPresent) { Status = STATUS_NOT_SUPPORTED; break; }
            Status = Identify(K->CipherKeyTypeInfo, K->Optional.PeerMacAddress_IsPresent ? &K->PeerMacAddress : NULL,
                Peer, K->Optional.CipherKeyID_IsPresent, K->CipherKeyID.CipherKeyID, &Changes[I]);
        }
        if (NT_SUCCESS(Status)) Status = Apply(Keys, D, Peer, Changes, Count, FALSE, Uncertain);
        CleanupParsedWdiSetDeleteCipherKeysToIhv(&P);
    }
    return Status;
}

NTSTATUS SwifiKeysClear(SWIFI_STATION_KEYS *Keys, SWIFI_DEVICE *D, const UCHAR *Peer)
{
    NTSTATUS Result = STATUS_SUCCESS;
    for (ULONG I = 0; I < RTL_NUMBER_OF(Keys->Slot); ++I) {
        SWIFI_KEY_SLOT *Slot = &Keys->Slot[I];
        if (Slot->Present) {
            NTSTATUS Status = SwifiJoinKey(&D->Hw.Control, Slot->Id, I == 4 ? Peer : NULL, NULL, NULL);
            if (!NT_SUCCESS(Status) && Status != STATUS_NOT_FOUND) Result = Status;
        }
    }
    RtlSecureZeroMemory(Keys, sizeof(*Keys));
    return Result;
}
