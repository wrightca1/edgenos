// Firmware mailbox function at 0x0177b524

void FUN_0177b524(char *param_1,int param_2,int *param_3)

{
  bool bVar1;
  int iVar2;
  int iVar3;
  uint uVar4;
  char *__s1;
  char *__s1_00;
  char *__s1_01;
  char *__s1_02;
  char *__s1_03;
  char *__s1_04;
  int iVar5;
  char *__s1_05;
  char *__s1_06;
  uint uVar6;
  undefined4 uVar7;
  
  iVar2 = DAT_0177b520;
  uVar7 = *(undefined4 *)(param_2 + 0x18);
  if ((*(uint *)(param_2 + 0x74) & 0x10) != 0) {
    FUN_013949c0(*(undefined4 *)(DAT_0177b520 + 0x177355c),*(int *)(DAT_0177b520 + 0x1773530) + 0x4c
                 ,param_1,*(undefined4 *)(param_2 + 4),*(undefined4 *)(param_2 + 8),
                 *(undefined4 *)(param_2 + 0xc),*(undefined4 *)(param_2 + 0x20),
                 *(undefined4 *)(param_2 + 0x10));
  }
  iVar5 = *(int *)(iVar2 + 0x1773530);
  uVar6 = 0;
  __s1_06 = *(char **)(iVar2 + 0x1773578);
  __s1_05 = *(char **)(iVar2 + 0x177357c);
  __s1_04 = *(char **)(iVar2 + 0x1773580);
  __s1_03 = *(char **)(iVar2 + 0x1773584);
  __s1_02 = *(char **)(iVar2 + 0x1773588);
  __s1_01 = *(char **)(iVar2 + 0x177358c);
  __s1_00 = *(char **)(iVar2 + 0x1773590);
  __s1 = *(char **)(iVar2 + 0x1773594);
  do {
    uVar4 = *(uint *)(param_2 + 0x10);
    if (uVar4 == 0xf) {
      if (uVar6 != 0) goto LAB_0177b5e4;
      uVar4 = *(uint *)(param_2 + 0x74);
joined_r0x0177bd24:
      if ((uVar4 & 0x10) != 0) {
        FUN_013949c0(*(undefined4 *)(iVar2 + 0x1773568),iVar5 + 0x4c,param_1,
                     *(undefined4 *)(*(int *)(iVar2 + 0x177356c) + *(int *)(param_2 + 0x10) * 4),
                     *(undefined4 *)(param_2 + 0x18),*(undefined4 *)(param_2 + 0x8c));
      }
      *(uint *)(param_2 + 0x18) = uVar6;
      iVar3 = FUN_0177b418(param_1,param_2);
      if (iVar3 != 3) {
        if (iVar3 == 0) goto LAB_0177b5fc;
        if (iVar3 == 1) {
          *param_3 = 0;
        }
        else if (iVar3 == -1) {
          FUN_013949c0(*(undefined4 *)(iVar2 + 0x1773570));
          *param_3 = -1;
        }
        else {
          FUN_013949c0(*(undefined4 *)(iVar2 + 0x1773574));
          *param_3 = -1;
        }
      }
      iVar3 = strcmp(__s1_06,param_1);
      if (iVar3 == 0) {
        iVar3 = FUN_017677e8(param_2);
LAB_0177bd60:
        if (iVar3 == 0) {
          if (*(int *)(param_2 + 0x10) != 0xf) goto LAB_0177b5fc;
          break;
        }
        *(undefined4 *)(param_2 + 0x18) = uVar7;
        *param_3 = iVar3;
        if (iVar3 == 1) {
          FUN_013949c0(*(undefined4 *)(iVar2 + 0x17736bc),*(int *)(iVar2 + 0x1773530) + 0x4c,param_1
                       ,*(undefined4 *)(param_2 + 4),*(undefined4 *)(param_2 + 8),
                       *(undefined4 *)(param_2 + 0xc));
          goto LAB_0177b60c;
        }
        iVar5 = *(int *)(iVar2 + 0x17736c8);
        if (iVar3 == -9) {
          FUN_013949c0(*(undefined4 *)(iVar2 + 0x17736c0),iVar5,param_1,*(undefined4 *)(param_2 + 4)
                       ,*(undefined4 *)(param_2 + 8),*(undefined4 *)(param_2 + 0xc));
          goto LAB_0177bf90;
        }
      }
      else {
        iVar3 = strcmp(__s1_05,param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017677e0(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(__s1_04,param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017677d8(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(__s1_03,param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01769580(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(__s1_02,param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017690a4(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(__s1_01,param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176e310(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(__s1_00,param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01769b4c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(__s1,param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017683f0(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773598),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176f640(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177359c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01768444(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735a0),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01769c44(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735a4),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176cdc8(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735a8),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017677d0(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735ac),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017677c8(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735b0),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770644(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735b4),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017677f0(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735b8),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01767c68(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735bc),param_1);
        if (iVar3 == 0) {
LAB_0177bee4:
          iVar3 = FUN_0176f708(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735c0),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01768290(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735c4),param_1);
        if (iVar3 == 0) goto LAB_0177bee4;
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735c8),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770d40(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735cc),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770cc4(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735d0),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770ccc(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735d4),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176a448(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735d8),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176a924(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735dc),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176a9c0(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735e0),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0177acc4(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735e4),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176ac20(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735e8),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01768bec(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735ec),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176d3a8(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735f0),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176837c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735f4),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176764c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735f8),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176aef4(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17735fc),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0177069c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773600),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176a30c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773604),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176e0dc(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773608),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176ad24(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177360c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176829c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773610),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01768524(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773614),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01768748(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773618),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176a7d4(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177361c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176a78c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773620),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176a5fc(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773624),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176a68c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773628),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176a718(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177362c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176a91c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773630),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770ca4(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773634),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770614(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773638),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0177061c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177363c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770624(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773640),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0177062c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773644),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770634(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773648),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0177060c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177364c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0177063c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773650),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176d604(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773654),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176909c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773658),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176f0b4(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177365c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017694a0(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773660),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176ab54(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773664),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01768e68(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773668),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770cac(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177366c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017702a4(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773670),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770508(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773674),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01768cf8(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773678),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770c9c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177367c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176b3d0(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773680),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176baa8(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773684),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770204(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773688),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770288(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177368c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770290(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773690),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770298(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773694),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770d48(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x1773698),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770d50(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x177369c),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017676d4(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17736a0),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_017706a8(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17736a4),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176b6cc(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17736a8),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176c96c(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17736ac),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_0176fd38(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17736b0),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01769de4(param_2);
          goto LAB_0177bd60;
        }
        iVar3 = strcmp(*(char **)(iVar2 + 0x17736b4),param_1);
        if (iVar3 == 0) {
          iVar3 = FUN_01770ce4(param_2);
          goto LAB_0177bd60;
        }
        iVar5 = *(int *)(iVar2 + 0x1773530) + 0x4c;
        FUN_013949c0(*(undefined4 *)(iVar2 + 0x17736b8),iVar5,*(undefined4 *)(param_2 + 4),
                     *(undefined4 *)(param_2 + 8),param_1);
        *(undefined4 *)(param_2 + 0x18) = uVar7;
        iVar3 = -1;
        *param_3 = -1;
      }
      FUN_013949c0(*(undefined4 *)(iVar2 + 0x17736c4),iVar5,param_1,iVar3,
                   *(undefined4 *)(param_2 + 4),*(undefined4 *)(param_2 + 8),
                   *(undefined4 *)(param_2 + 0xc));
LAB_0177bf90:
      *param_3 = -1;
      return;
    }
    if (0xf < uVar4) {
      FUN_013949c0(*(undefined4 *)(iVar2 + 0x1773560),iVar5 + 100);
LAB_0177b674:
      uVar4 = *(uint *)(param_2 + 0x74);
      goto joined_r0x0177bd24;
    }
LAB_0177b5e4:
    if ((*(int *)(*(int *)(iVar2 + 0x1773564) + uVar4 * 4) >> (uVar6 & 0x3f) & 1U) != 0)
    goto LAB_0177b674;
LAB_0177b5fc:
    bVar1 = uVar6 != 3;
    uVar6 = uVar6 + 1;
  } while (bVar1);
  *(undefined4 *)(param_2 + 0x18) = uVar7;
LAB_0177b60c:
  *param_3 = 0;
  return;
}


