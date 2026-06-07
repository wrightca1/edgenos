// program: libopennsl.so.1

// ===== 0x158f010 =====
// FUN_0158f010

int FUN_0158f010(undefined4 param_1,int param_2,uint param_3,uint param_4,undefined4 param_5)

{
  bool bVar1;
  int iVar2;
  int iVar3;
  
  param_3 = param_3 & 0xf;
  iVar2 = *(int *)(param_2 + 0x1d4);
  if ((4 < param_3) && (param_3 != 0xf)) {
    param_3 = 1;
  }
  iVar3 = 0;
  do {
    bVar1 = iVar3 != 2;
    if (((uint)*(ushort *)(*(int *)(DAT_0158f00c + 0x1587044) + iVar3 * 4) <= (param_4 & 0xffff)) &&
       ((param_4 & 0xffff) <= (uint)*(ushort *)(*(int *)(DAT_0158f00c + 0x1587044) + iVar3 * 4 + 2))
       ) {
      param_3 = 1;
      if (iVar2 != 0) goto LAB_0158f130;
      goto LAB_0158f17c;
    }
    iVar3 = iVar3 + 1;
  } while (bVar1);
  if (iVar2 == 0) {
    if (param_3 == 0) {
      if ((*(uint *)(param_2 + 0x100) & 1) != 0) {
        param_4 = param_4 | (uint)*(byte *)(param_2 + 0x17) << 0x10;
      }
      goto joined_r0x0158f168;
    }
    if (param_3 == 0xf) {
      param_4 = param_4 | 0x1ff0000;
    }
    else {
LAB_0158f17c:
      param_4 = param_4 | (param_3 - 1) * 0x10000;
    }
  }
  else {
LAB_0158f130:
    param_3 = 1;
    param_4 = param_4 | (iVar2 + 0x1ff) * 0x10000;
  }
  if ((*(uint *)(param_2 + 0x100) & 1) == 0) {
    *(ushort *)(param_2 + 0x14) = *(short *)(param_2 + 0x14) - (ushort)*(byte *)(param_2 + 0x17);
  }
joined_r0x0158f168:
  if ((param_4 & 0xffff) == 0xffde) {
    param_4 = 0xffde;
  }
  iVar2 = FUN_0156c428(param_1,param_2,param_4,param_5);
  if (((-1 < iVar2) && (iVar2 = 0, param_3 != 0)) && ((*(uint *)(param_2 + 0x100) & 1) == 0)) {
    *(ushort *)(param_2 + 0x14) = (ushort)*(byte *)(param_2 + 0x17) + *(short *)(param_2 + 0x14);
  }
  return iVar2;
}



// ===== 0x158c5b0 =====
// FUN_0158c5b0

int FUN_0158c5b0(undefined4 param_1,int param_2,uint param_3,uint param_4,undefined2 *param_5)

{
  bool bVar1;
  uint uVar2;
  int iVar3;
  undefined2 local_28 [12];
  
  param_3 = param_3 & 0xf;
  if (4 < param_3) {
    param_3 = 1;
  }
  iVar3 = 0;
  while( true ) {
    bVar1 = iVar3 == 2;
    if (((uint)*(ushort *)(*(int *)(DAT_0158c5ac + 0x15845e4) + iVar3 * 4) <= (param_4 & 0xffff)) &&
       ((param_4 & 0xffff) <= (uint)*(ushort *)(*(int *)(DAT_0158c5ac + 0x15845e4) + iVar3 * 4 + 2))
       ) break;
    iVar3 = iVar3 + 1;
    if (bVar1) {
      uVar2 = (param_3 - 1) * 0x10000;
      if (param_3 == 0) {
        uVar2 = param_4;
        if ((*(uint *)(param_2 + 0x100) & 1) != 0) {
          uVar2 = param_4 | (uint)*(byte *)(param_2 + 0x17) << 0x10;
          param_4 = uVar2;
        }
      }
      else {
LAB_0158c6c8:
        if ((*(uint *)(param_2 + 0x100) & 1) == 0) {
          *(ushort *)(param_2 + 0x14) =
               *(short *)(param_2 + 0x14) - (ushort)*(byte *)(param_2 + 0x17);
        }
        uVar2 = param_4 | uVar2;
      }
      if ((param_4 & 0xffff) == 0xffde) {
        uVar2 = 0xffde;
      }
      iVar3 = FUN_0156c414(param_1,param_2,uVar2,local_28);
      if (-1 < iVar3) {
        if ((param_3 != 0) && ((*(uint *)(param_2 + 0x100) & 1) == 0)) {
          *(ushort *)(param_2 + 0x14) =
               (ushort)*(byte *)(param_2 + 0x17) + *(short *)(param_2 + 0x14);
        }
        iVar3 = 0;
        *param_5 = local_28[0];
      }
      return iVar3;
    }
  }
  uVar2 = 0;
  param_3 = 1;
  goto LAB_0158c6c8;
}



// ===== 0x158ee1c =====
// FUN_0158ee1c

undefined4
FUN_0158ee1c(undefined4 *param_1,undefined4 param_2,ushort param_3,int param_4,undefined4 param_5,
            undefined4 param_6)

{
  bool bVar1;
  int iVar2;
  byte in_cr0;
  byte in_cr1;
  byte unaff_cr2;
  byte unaff_cr3;
  byte unaff_cr4;
  byte in_cr5;
  byte in_cr6;
  byte in_cr7;
  ushort local_48 [2];
  undefined1 auStack_44 [40];
  uint local_1c;
  
  local_1c = (uint)(in_cr0 & 0xf) << 0x1c | (uint)(in_cr1 & 0xf) << 0x18 |
             (uint)(unaff_cr2 & 0xf) << 0x14 | (uint)(unaff_cr3 & 0xf) << 0x10 |
             (uint)(unaff_cr4 & 0xf) << 0xc | (uint)(in_cr5 & 0xf) << 8 | (uint)(in_cr6 & 0xf) << 4
             | (uint)(in_cr7 & 0xf);
  local_48[0] = 0;
  FUN_0190b870(auStack_44,param_5,0);
  do {
    iVar2 = FUN_0158c5b0(*param_1,param_1,param_6,param_2,local_48);
    bVar1 = (param_3 & local_48[0]) == 0;
    if (param_4 == 0) {
      if (bVar1) {
        return 0;
      }
    }
    else if (!bVar1) {
      return 0;
    }
  } while ((-1 < iVar2) && (iVar2 = FUN_0190b888(auStack_44), iVar2 == 0));
  return 0xfffffff7;
}


