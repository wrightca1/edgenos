
int FUN_01597f00(undefined4 param_1,int param_2,int param_3)

{
  byte bVar1;
  bool bVar2;
  uint uVar3;
  int iVar4;
  undefined4 uVar5;
  
  uVar3 = *(uint *)(param_2 + 0x1e8);
  if (uVar3 != 0) {
    uVar5 = 0;
    if (param_3 != 0) {
      uVar5 = 0x20;
    }
    if ((*(char *)(param_2 + 0x18) == '\x02') || (*(char *)(param_2 + 0x18) == '\x05')) {
      if (uVar3 == 1) {
        bVar2 = true;
LAB_015980bc:
        iVar4 = FUN_0158fbdc(param_1,param_2,0,(uint)*(byte *)(param_2 + 0x17) * 0x10 + 0x8061,uVar5
                             ,0x20);
        if (iVar4 < 0) {
          return iVar4;
        }
      }
      else {
        bVar2 = (uVar3 & 0xf0) == 0xf0;
        if ((uVar3 & 0xf) == 0xf) goto LAB_015980bc;
      }
      if ((bVar2) &&
         (iVar4 = FUN_0158fbdc(param_1,param_2,0,(*(byte *)(param_2 + 0x17) + 1) * 0x10 + 0x8061,
                               uVar5,0x20), iVar4 < 0)) {
        return iVar4;
      }
    }
    else if ((uVar3 == 1) || ((uVar3 & 0xf) == 0xf)) {
      bVar2 = false;
      goto LAB_015980bc;
    }
  }
  uVar3 = *(uint *)(param_2 + 0x1ec);
  if (uVar3 == 0) {
    return 0;
  }
  bVar1 = ~-(param_3 == 0) & 0xc;
  if ((*(char *)(param_2 + 0x18) == '\x02') || (*(char *)(param_2 + 0x18) == '\x05')) {
    if (uVar3 == 1) {
      bVar2 = true;
    }
    else {
      bVar2 = (uVar3 & 0xf0) == 0xf0;
      if ((uVar3 & 0xf) != 0xf) goto LAB_0159800c;
    }
  }
  else {
    if ((uVar3 != 1) && ((uVar3 & 0xf) != 0xf)) {
      return 0;
    }
    bVar2 = false;
  }
  iVar4 = FUN_0158fbdc(param_1,param_2,0,(uint)*(byte *)(param_2 + 0x17) * 0x10 + 0x80ba,bVar1,0xc);
  if (iVar4 < 0) {
    return iVar4;
  }
LAB_0159800c:
  iVar4 = 0;
  if ((bVar2) &&
     (iVar4 = FUN_0158fbdc(param_1,param_2,0,(*(byte *)(param_2 + 0x17) + 1) * 0x10 + 0x80ba,bVar1,
                           0xc), 0 < iVar4)) {
    iVar4 = 0;
  }
  return iVar4;
}


