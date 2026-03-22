// WC40_REG_MODIFY at 0x01752890

/* WARNING: Type propagation algorithm not settling */

undefined4
FUN_01752890(int param_1,int param_2,int param_3,undefined4 param_4,uint param_5,uint param_6)

{
  int iVar1;
  int iVar2;
  undefined4 uVar3;
  uint uVar4;
  undefined4 uVar5;
  undefined4 uVar6;
  undefined4 uVar7;
  uint uVar8;
  int iVar9;
  int iVar10;
  undefined4 local_38 [4];
  
  iVar1 = DAT_0175288c;
  iVar10 = *(int *)(*(int *)(*(int *)(DAT_0175288c + 0x174a8a0) + param_1 * 4) + param_2 * 4);
  local_38[0] = 0;
  uVar3 = 0xfffffffc;
  uVar8 = (uint)*(byte *)(iVar10 + 0x17) + param_3;
  iVar2 = ((int)uVar8 >> 2) + (uint)((int)uVar8 < 0 && (uVar8 & 3) != 0);
  uVar4 = *(int *)(*(int *)(*(int *)(DAT_0175288c + 0x174a97c) + param_1 * 4) +
                   (*(int *)(iVar10 + 4) + 0x3314) * 4 + 0xc) + 3;
  if (iVar2 < (int)(((int)uVar4 >> 2) + (uint)((int)uVar4 < 0 && (uVar4 & 3) != 0))) {
    iVar9 = uVar8 + iVar2 * -4;
    iVar2 = iVar10 + 0x4d0 + iVar2 * 0xe4;
    uVar5 = *(undefined4 *)(iVar2 + 0x18);
    uVar6 = *(undefined4 *)(iVar2 + 0x10);
    uVar7 = *(undefined4 *)(iVar2 + 0x8c);
    uVar3 = FUN_0174dde0(*(undefined4 *)(iVar2 + 4),*(undefined4 *)(iVar2 + 8),iVar9);
    *(undefined4 *)(iVar2 + 0x10) = uVar3;
    *(int *)(iVar2 + 0x18) = iVar9;
    uVar4 = param_5 & 0xf | 0x7000000 | (param_5 & 0x3f0) << 4 | (param_5 & 0x7c00) << 6;
    if ((param_6 & 0x20) != 0) {
      if ((param_5 & 0x8000) == 0) {
        uVar3 = *(undefined4 *)(iVar1 + 0x174a9e0);
        *(undefined4 *)(iVar10 + 0x4f0) = 0;
        FUN_0177b524(uVar3,iVar2,local_38);
        *(uint *)(iVar10 + 0x4f0) = uVar4;
      }
      else {
        uVar3 = *(undefined4 *)(iVar1 + 0x174a9e0);
        *(uint *)(iVar10 + 0x4f0) = uVar4 | 0x8000000;
      }
      FUN_0177b524(uVar3,iVar2,local_38);
    }
    if ((param_6 & 1) != 0) {
      uVar3 = *(undefined4 *)(iVar1 + 0x174a9e0);
      *(uint *)(iVar10 + 0x4f0) = uVar4 | 0x10000000;
      FUN_0177b524(uVar3,iVar2,local_38);
    }
    if ((param_6 & 2) != 0) {
      uVar3 = *(undefined4 *)(iVar1 + 0x174a9e0);
      *(uint *)(iVar10 + 0x4f0) = uVar4 | 0x20000000;
      FUN_0177b524(uVar3,iVar2,local_38);
    }
    if ((param_6 & 4) != 0) {
      uVar3 = *(undefined4 *)(iVar1 + 0x174a9e0);
      *(uint *)(iVar10 + 0x4f0) = uVar4 | 0x30000000;
      FUN_0177b524(uVar3,iVar2,local_38);
    }
    if ((param_6 & 8) != 0) {
      uVar3 = *(undefined4 *)(iVar1 + 0x174a9e0);
      *(uint *)(iVar10 + 0x4f0) = uVar4 | 0x40000000;
      FUN_0177b524(uVar3,iVar2,local_38);
    }
    if ((param_6 & 0x10) != 0) {
      uVar3 = *(undefined4 *)(iVar1 + 0x174a9e0);
      *(uint *)(iVar10 + 0x4f0) = uVar4 | 0x50000000;
      FUN_0177b524(uVar3,iVar2,local_38);
    }
    *(undefined4 *)(iVar2 + 0x18) = uVar5;
    uVar3 = 0;
    *(undefined4 *)(iVar2 + 0x10) = uVar6;
    *(undefined4 *)(iVar2 + 0x8c) = uVar7;
  }
  return uVar3;
}


