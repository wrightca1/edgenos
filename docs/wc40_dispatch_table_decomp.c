// Core register write at 0x0177b418

undefined4 FUN_0177b418(char *param_1,undefined4 param_2)

{
  bool bVar1;
  int iVar2;
  int iVar3;
  undefined4 uVar4;
  int *piVar5;
  char *__s2;
  int iVar6;
  
  iVar2 = DAT_0177b414;
  piVar5 = *(int **)(DAT_0177b414 + 0x1773438);
  __s2 = (char *)*piVar5;
  if (__s2 != (char *)0x0) {
    iVar6 = 0;
    while( true ) {
      iVar3 = strcmp(param_1,__s2);
      bVar1 = iVar6 == 99;
      if (iVar3 == 0) break;
      iVar6 = iVar6 + 1;
      if (bVar1) {
        return 3;
      }
      __s2 = (char *)piVar5[iVar6 * 2];
      if (__s2 == (char *)0x0) {
        return 3;
      }
    }
    FUN_013949c0(*(undefined4 *)(iVar2 + 0x177344c),__s2);
    if ((code *)piVar5[iVar6 * 2 + 1] != (code *)0x0) {
      uVar4 = (*(code *)piVar5[iVar6 * 2 + 1])(param_2);
      return uVar4;
    }
  }
  return 3;
}


