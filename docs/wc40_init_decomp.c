
int FUN_0159ecc0(int param_1,int param_2)

{
  int iVar1;
  int iVar2;
  uint uVar3;
  int iVar4;
  int iVar5;
  int iVar6;
  int iVar7;
  
  iVar2 = DAT_0159ecbc;
  FUN_0159e980();
  iVar7 = *(int *)(iVar2 + 0x1596ccc);
  iVar4 = param_1 * 4;
  iVar1 = param_2 * 0x18;
  uVar3 = *(uint *)(*(int *)(iVar7 + iVar4) + iVar1 + 0xc);
  iVar6 = *(int *)(*(int *)(*(int *)(iVar2 + 0x1596cd0) + iVar4) + param_2 * 4);
  if ((uVar3 & 0x1000) == 0) {
    iVar5 = FUN_015591f0(param_1,param_2);
    if (iVar5 < 0) {
      return iVar5;
    }
    uVar3 = *(uint *)(*(int *)(iVar7 + iVar4) + iVar1 + 0xc);
  }
  if ((uVar3 & 0x10000) == 0) {
    iVar6 = FUN_0159ca04(param_1,iVar6);
    if (iVar6 < 0) {
      return iVar6;
    }
    iVar6 = FUN_01599350(param_1,param_2);
  }
  else if (*(int *)(iVar6 + 0x1d8) == 0) {
    iVar6 = FUN_0159cbcc();
  }
  else {
    iVar6 = FUN_0159d048(param_1,param_2);
  }
  if (-1 < iVar6) {
    if (((*(uint *)(*(int *)(iVar7 + iVar4) + iVar1 + 0xc) & 0x1000) == 0) &&
       (*(int *)(*(int *)(iVar2 + 0x1596db4) + iVar4) != 1)) {
      FUN_015942e4(param_1,param_2,0);
    }
    iVar6 = 0;
    iVar4 = FUN_01393e80(0xa00fe04);
    if (iVar4 != 0) {
      FUN_013949c0(*(undefined4 *)(iVar2 + 0x1596e34),0xa00fe04,*(undefined4 *)(iVar2 + 0x1596cdc),
                   0xd0f,*(int *)(iVar2 + 0x1596cf8) + 0x404,param_1,param_1,param_2);
    }
  }
  return iVar6;
}


