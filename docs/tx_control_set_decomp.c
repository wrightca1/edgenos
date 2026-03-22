// _phy_wc40_tx_control_set at 0x0175308c

undefined4 FUN_0175308c(int param_1,int param_2,ushort *param_3)

{
  undefined1 uVar1;
  undefined1 uVar2;
  undefined1 uVar3;
  bool bVar4;
  bool bVar5;
  bool bVar6;
  bool bVar7;
  bool bVar8;
  undefined4 uVar9;
  undefined4 uVar10;
  int iVar11;
  ushort uVar12;
  int iVar13;
  int iVar14;
  int iVar15;
  
  iVar13 = *(int *)(*(int *)(*(int *)(DAT_01753088 + 0x174b09c) + param_1 * 4) + param_2 * 4);
  iVar11 = *(int *)(*(int *)(*(int *)(DAT_01753088 + 0x174b178) + param_1 * 4) +
                    (*(int *)(iVar13 + 4) + 0x3314) * 4 + 0xc);
  if (0 < iVar11) {
    uVar10 = *(undefined4 *)(DAT_01753088 + 0x174b1e8);
    iVar14 = 0;
    do {
      while( true ) {
        uVar1 = *(undefined1 *)((int)param_3 + 0x19);
        uVar2 = *(undefined1 *)(param_3 + 0xd);
        uVar3 = *(undefined1 *)(param_3 + 0xc);
        uVar12 = *param_3;
        FUN_01752d64(param_1,param_2,iVar14,2,uVar1);
        FUN_01752d64(param_1,param_2,iVar14,3,uVar2);
        FUN_01752d64(param_1,param_2,iVar14,0x62,uVar3);
        if (*(int *)(iVar13 + 0x520) == 0) break;
        if ((*(int *)(iVar13 + 0x1c8) == 0) && (*(int *)(iVar13 + 0x1d0) < 1)) goto LAB_01753250;
        uVar12 = uVar12 & 0x7fff;
        if (*(int *)(iVar13 + 0x1d4) != 0) goto LAB_01753164;
LAB_0175325c:
        bVar8 = false;
        bVar7 = false;
        bVar6 = false;
        bVar5 = false;
        bVar4 = false;
        uVar9 = 0x20;
        if ((*(uint *)(iVar13 + 0x544) & 0x2000) == 0) goto LAB_01753188;
LAB_01753280:
        FUN_013949c0(uVar10,param_1,param_2,iVar14,*(undefined1 *)(iVar13 + 0x17),uVar12,uVar1,uVar2
                     ,uVar3,*(undefined4 *)(iVar13 + 0x1d4),uVar9);
        FUN_01752890(param_1,param_2,iVar14,1,uVar12,0x20);
        if (!bVar4) goto LAB_017531ac;
LAB_017532d8:
        FUN_01752890(param_1,param_2,iVar14,1,param_3[2],1);
        if (!bVar5) goto LAB_017531b4;
LAB_017532fc:
        FUN_01752890(param_1,param_2,iVar14,1,param_3[4],2);
        if (!bVar6) goto LAB_017531bc;
LAB_01753320:
        FUN_01752890(param_1,param_2,iVar14,1,param_3[6],4);
        if (!bVar7) goto LAB_017531c4;
LAB_01753344:
        FUN_01752890(param_1,param_2,iVar14,1,param_3[8],8);
        if (!bVar8) goto LAB_017531cc;
LAB_01753368:
        iVar15 = iVar14 + 1;
        FUN_01752890(param_1,param_2,iVar14,1,param_3[10],0x10);
        param_3 = param_3 + 0xe;
        iVar14 = iVar15;
        if (iVar15 == iVar11) {
          return 0;
        }
      }
      if (*(short *)(iVar13 + 0x4c2) != 0) {
        uVar12 = uVar12 & 0x7fff;
      }
LAB_01753250:
      if (*(int *)(iVar13 + 0x1d4) == 0) goto LAB_0175325c;
LAB_01753164:
      bVar8 = true;
      bVar7 = true;
      bVar6 = true;
      bVar5 = true;
      bVar4 = true;
      uVar9 = 0x3f;
      if ((*(uint *)(iVar13 + 0x544) & 0x2000) != 0) goto LAB_01753280;
LAB_01753188:
      FUN_01752890(param_1,param_2,iVar14,1,uVar12,0x20);
      if (bVar4) goto LAB_017532d8;
LAB_017531ac:
      if (bVar5) goto LAB_017532fc;
LAB_017531b4:
      if (bVar6) goto LAB_01753320;
LAB_017531bc:
      if (bVar7) goto LAB_01753344;
LAB_017531c4:
      if (bVar8) goto LAB_01753368;
LAB_017531cc:
      iVar14 = iVar14 + 1;
      param_3 = param_3 + 0xe;
    } while (iVar14 != iVar11);
  }
  return 0;
}


