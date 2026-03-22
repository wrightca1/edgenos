// WC40_REG_MODIFY (ind_lane version) at 0x0158fbdc

int FUN_0158fbdc(undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
                ushort param_5,ushort param_6)

{
  int iVar1;
  undefined2 local_38;
  
  iVar1 = FUN_0158c5b0();
  if (-1 < iVar1) {
    iVar1 = FUN_0158f010(param_1,param_2,param_3,param_4,local_38 & ~param_6 | param_6 & param_5);
    if (0 < iVar1) {
      iVar1 = 0;
    }
  }
  return iVar1;
}


