
int FUN_01597254(int param_1,int param_2,uint param_3)

{
  int iVar1;
  undefined4 uVar2;
  undefined4 uVar3;
  undefined4 uVar4;
  int iVar5;
  uint uVar6;
  int iVar7;
  uint uVar8;
  uint uVar9;
  int iVar10;
  
  iVar1 = DAT_01597250;
  iVar7 = -4;
  if (param_3 < 8) {
    uVar8 = 0;
    iVar10 = *(int *)(*(int *)(*(int *)(DAT_01597250 + 0x158f264) + param_1 * 4) + param_2 * 4);
    uVar6 = 3;
    if (*(int *)(iVar10 + 0x1c4) != 0xc) {
      uVar8 = (uint)*(byte *)(iVar10 + 0x17);
      if ((*(char *)(iVar10 + 0x18) == '\x02') ||
         (uVar6 = uVar8, *(char *)(iVar10 + 0x18) == '\x05')) {
        uVar6 = uVar8 + 1;
      }
    }
    iVar5 = *(int *)(DAT_01597250 + 0x158f28c);
    uVar9 = uVar8 << 2;
    uVar4 = *(undefined4 *)(DAT_01597250 + 0x158f33c);
    uVar3 = *(undefined4 *)(DAT_01597250 + 0x158f270);
    do {
      uVar2 = *(undefined4 *)(iVar5 + 200 + uVar9);
      iVar7 = FUN_0158fbdc(param_1,iVar10,uVar2,0x820e,0x301,0xff0f);
      if (iVar7 < 0) {
        return iVar7;
      }
      iVar7 = FUN_0158ee1c(iVar10,0x820e,0x80,1,250000,uVar2);
      if (iVar7 == -9) {
        iVar7 = FUN_01393e80(0xa00fe03);
        if (iVar7 == 0) {
          return -9;
        }
        uVar3 = *(undefined4 *)(iVar1 + 0x158f330);
        uVar4 = *(undefined4 *)(iVar1 + 0x158f270);
        uVar2 = 0x2fc;
LAB_01597628:
        FUN_013949c0(uVar3,0xa00fe03,uVar4,uVar2,iVar5 + 0x2a4,param_1,param_1,param_2,uVar8);
        return -9;
      }
      iVar7 = FUN_0158fbdc(param_1,iVar10,uVar2,0x820e,1,0xff0f);
      if (iVar7 < 0) {
        return iVar7;
      }
      FUN_0137fa34(1000);
      iVar7 = FUN_0158ee1c(iVar10,0x820e,0x80,1,250000,uVar2);
      if (iVar7 == -9) {
        iVar7 = FUN_01393e80(0xa00fe03);
        if (iVar7 == 0) {
          return -9;
        }
        uVar3 = *(undefined4 *)(iVar1 + 0x158f334);
        uVar4 = *(undefined4 *)(iVar1 + 0x158f270);
        uVar2 = 0x313;
        goto LAB_01597628;
      }
      iVar7 = FUN_0158fbdc(param_1,iVar10,1,0x81f2,param_3 << (uVar9 & 0x3f) & 0xffff,
                           0xf << (uVar9 & 0x3f) & 0xffff);
      if (iVar7 < 0) {
        return iVar7;
      }
      iVar7 = FUN_0158fbdc(param_1,iVar10,uVar2,0x820e,0x201,0xff0f);
      if (iVar7 < 0) {
        return iVar7;
      }
      FUN_0137fa34(1000);
      iVar7 = FUN_0158ee1c(iVar10,0x820e,0x80,1,250000,uVar2);
      if (iVar7 == -9) {
        iVar7 = FUN_01393e80(0xa00fe03);
        if (iVar7 == 0) {
          return -9;
        }
        uVar3 = *(undefined4 *)(iVar1 + 0x158f338);
        uVar4 = *(undefined4 *)(iVar1 + 0x158f270);
        uVar2 = 0x332;
        goto LAB_01597628;
      }
      iVar7 = FUN_0158fbdc(param_1,iVar10,uVar2,0x820e,0x301,0xff0f);
      if (iVar7 < 0) {
        return iVar7;
      }
      FUN_0137fa34(1000);
      iVar7 = FUN_0158ee1c(iVar10,0x820e,0x80,1,250000,uVar2);
      if ((iVar7 == -9) && (iVar7 = FUN_01393e80(0xa00fe03), iVar7 != 0)) {
        FUN_013949c0(uVar4,0xa00fe03,uVar3,0x34a,iVar5 + 0x2a4,param_1,param_1,param_2,uVar8);
      }
      uVar8 = uVar8 + 1;
      uVar9 = uVar9 + 4;
    } while ((int)uVar8 <= (int)uVar6);
    iVar7 = 0;
    if (((*(int *)(iVar10 + 0x28c) == 1) && (iVar10 = FUN_01596fcc(param_1,param_2,1), iVar10 != 0))
       && (iVar10 = FUN_01393e80(0xa00fe03), iVar10 != 0)) {
      FUN_013949c0(*(undefined4 *)(iVar1 + 0x158f340),0xa00fe03,*(undefined4 *)(iVar1 + 0x158f270),
                   0x352,iVar5 + 0x2a4,param_1,iVar5 + 0x2a4,param_1,param_2);
    }
  }
  return iVar7;
}


