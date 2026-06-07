// program: libopennsl.so.1
// image base: 0x00010000

// ================================================
// FUN_015936ac @ 0x15936ac  (entry 0x015936ac)
// ================================================

int FUN_015936ac(int param_1,int param_2,int param_3,short *param_4,undefined4 *param_5)

{
  bool bVar1;
  int iVar2;
  uint uVar3;
  short sVar4;
  int iVar5;
  ushort uVar6;
  
  iVar5 = *(int *)(*(int *)(*(int *)(DAT_015936a8 + 0x158b6bc) + param_1 * 4) + param_2 * 4);
  iVar2 = *(int *)(iVar5 + 0x1fc);
  *param_4 = 0;
  bVar1 = iVar2 != 0;
  if (param_3 == 11000) {
LAB_015937c0:
    uVar3 = *(uint *)(iVar5 + 0x1dc);
    if ((uVar3 & 0x12200) == 0) {
      *param_4 = 0x25;
      iVar2 = 0;
      if ((uVar3 & 0x400) == 0) {
        *param_5 = 0;
      }
      else {
        *param_5 = 0;
      }
    }
    else {
      *param_4 = 0x29;
      iVar2 = 0;
    }
  }
  else {
    if (param_3 < 0x2af9) {
      if (param_3 == 3000) {
LAB_01593754:
        *param_4 = 0x10;
        *param_5 = 6;
        return 0;
      }
      if (param_3 == 10000) {
        if ((*(char *)(iVar5 + 0x18) == '\x02') || (*(char *)(iVar5 + 0x18) == '\x05')) {
          if (*(int *)(iVar5 + 0x1e4) != 0) {
            *param_4 = 0x30;
            return 0;
          }
          if (!bVar1) {
            *param_4 = (-(ushort)(*(int *)(iVar5 + 0x1e0) == 0) & 0xfff2) + 0x2e;
            *param_5 = 5;
            return 0;
          }
          *param_4 = 0x21;
          return 0;
        }
        goto LAB_015937c0;
      }
      if (param_3 == 0x9c4) goto LAB_01593754;
    }
    else {
      if (param_3 == 15000) {
        *param_4 = 0x36;
        return 0;
      }
      if (param_3 < 0x3a99) {
        if (param_3 == 12000) {
          if (*(int *)(iVar5 + 0x1e4) != 0) {
            *param_4 = 0x2f;
            return 0;
          }
          if (bVar1) {
            *param_4 = 0x23;
            return 0;
          }
          *param_4 = 0x24;
          return 0;
        }
      }
      else {
        if (param_3 == 20000) {
          sVar4 = 0x28;
          if (bVar1) {
            sVar4 = 0x27;
          }
          *param_4 = sVar4;
          if ((*(uint *)(iVar5 + 0x1dc) & 0x800) == 0) {
            return 0;
          }
          *param_4 = 0x3f;
          uVar6 = *(ushort *)(iVar5 + 0x2a4) & 0xf800;
          if ((uVar6 != 0x4000) && (uVar6 != 0x4800)) {
            return 0;
          }
          iVar2 = FUN_0158fbdc(param_1,iVar5,0,0x833d,0,0x8000);
          if (iVar2 < 1) {
            return iVar2;
          }
          return 0;
        }
        if (param_3 == 21000) {
          *param_4 = 0x27;
          return 0;
        }
      }
    }
    iVar2 = -4;
  }
  return iVar2;
}



// ================================================
// FUN_0158b954 @ 0x158b954  (entry 0x0158b954)
// ================================================

undefined4 FUN_0158b954(int param_1,uint param_2,undefined4 *param_3)

{
  char cVar1;
  int iVar2;
  int iVar3;
  uint uVar4;
  undefined4 uVar5;
  uint uVar6;
  int iVar7;
  int iVar8;
  int iVar9;
  int iVar10;
  int iVar11;
  int iVar12;
  
  iVar3 = DAT_0158b950;
  if (param_3 == (undefined4 *)0x0) {
    return 0xfffffffc;
  }
  iVar11 = *(int *)(FUN_01583960 + DAT_0158b950 + 4);
  iVar7 = param_1 * 4;
  iVar2 = param_2 * 4;
  iVar12 = *(int *)(*(int *)(iVar11 + iVar7) + iVar2);
  memset(param_3,0,0x30);
  cVar1 = *(char *)(iVar12 + 0x18);
  iVar8 = *(int *)(iVar12 + 0x1bc);
  if ((cVar1 == '\x04') || (cVar1 == '\x06')) {
    param_3[1] = 0x100;
    param_3[4] = 2;
    param_3[5] = 4;
    return 0;
  }
  if (cVar1 == '\x05') {
    param_3[1] = 0x2000;
    param_3[4] = 2;
    param_3[5] = 4;
    param_3[3] = param_3[3] | 0x20;
    return 0;
  }
  param_3[10] = 3;
  iVar9 = (int)param_2 >> 5;
  if (*(int *)(iVar12 + 0x1c4) - 4U < 2) {
    param_3[1] = 0x40;
    if (*(int *)(iVar12 + 0x1a0) == 0) {
      *param_3 = 0x21;
      param_3[1] = 0x61;
    }
    else {
      uVar5 = 0x60;
      if (iVar8 == 1) {
        uVar5 = 0xe0;
      }
      param_3[1] = uVar5;
      *param_3 = 0x20;
    }
    iVar8 = *(int *)(iVar12 + 0x1c);
    if (iVar8 == 13000) {
      uVar4 = param_3[1];
LAB_0158bdc0:
      uVar4 = uVar4 | 0xa000;
LAB_0158bdc4:
      uVar4 = uVar4 | 0x1000;
LAB_0158bdc8:
      iVar10 = *(int *)(iVar3 + 0x1583968);
      param_3[1] = uVar4 | 0x800;
      iVar8 = *(int *)(iVar10 + iVar7);
      if ((iVar8 != 0) && ((*(uint *)((int)&DAT_00a37198 + iVar8) & 0x800) != 0)) {
        param_3[1] = uVar4 | 0x1800;
        iVar8 = *(int *)(iVar10 + iVar7);
      }
      iVar9 = iVar9 + (uint)((int)param_2 < 0 && (param_2 & 0x1f) != 0);
      if ((1 << (param_2 + iVar9 * -0x20 & 0x3f) & *(uint *)(iVar8 + (iVar9 + 0x438) * 4 + 0x10)) !=
          0) {
        param_3[1] = param_3[1] | 0xe0;
      }
    }
    else {
      if (13000 < iVar8) {
        if (iVar8 == 20000) {
          uVar4 = param_3[1];
LAB_0158beb4:
          uVar4 = uVar4 | 0xc0000;
        }
        else {
          if (iVar8 == 21000) {
            uVar4 = param_3[1];
            goto LAB_0158beb4;
          }
          if (iVar8 != 15000) goto LAB_0158bc44;
          uVar4 = param_3[1];
        }
        uVar4 = uVar4 | 0x10000;
        goto LAB_0158bdc0;
      }
      if (iVar8 == 11000) {
        uVar4 = param_3[1];
        goto LAB_0158bdc4;
      }
      if (iVar8 == 12000) {
        uVar4 = param_3[1] | 0x2000;
        goto LAB_0158bdc4;
      }
      if (iVar8 == 10000) {
        uVar4 = param_3[1];
        goto LAB_0158bdc8;
      }
    }
LAB_0158bc44:
    param_3[2] = 7;
    param_3[3] = 0x14;
    if (9999 < *(int *)(iVar12 + 0x1c)) {
      param_3[3] = 0x34;
    }
    uVar5 = *(undefined4 *)(*(int *)(*(int *)(iVar11 + iVar7) + iVar2) + 0x1f8);
    param_3[5] = 4;
    param_3[4] = uVar5;
    if ((*(char *)(iVar12 + 0x18) == '\x02') || (*(char *)(iVar12 + 0x18) == '\x05')) {
      param_3[6] = 0;
    }
    else {
      param_3[6] = 1;
    }
    goto LAB_0158bac8;
  }
  if ((*(int *)(iVar12 + 0x1c) < 0x2711) ||
     (iVar8 = iVar9 + (uint)((int)param_2 < 0 && (param_2 & 0x1f) != 0),
     (1 << (param_2 + iVar8 * -0x20 & 0x3f) &
     *(uint *)(*(int *)(*(int *)(iVar3 + 0x1583968) + iVar7) + (iVar8 + 0x438) * 4 + 0x10)) != 0)) {
    *param_3 = 0;
    param_3[1] = 0x40;
    if (*(int *)(iVar12 + 0x1a0) == 0) {
      *param_3 = 0x21;
      uVar4 = 0x61;
      param_3[1] = 0x61;
      goto LAB_0158ba50;
    }
    param_3[1] = 0xe0;
    *param_3 = 0x20;
    uVar4 = 0xe0;
    iVar8 = *(int *)(iVar12 + 0x1c);
    if (iVar8 != 21000) goto LAB_0158ba5c;
LAB_0158bb84:
    uVar4 = uVar4 | 0xc0000;
LAB_0158ba80:
    uVar4 = uVar4 | 0x20000;
LAB_0158ba84:
    uVar4 = uVar4 | 0x18000;
LAB_0158ba8c:
    uVar4 = uVar4 | 0x2000;
  }
  else {
    param_3[1] = 0;
    *param_3 = 0;
    uVar4 = 0;
LAB_0158ba50:
    iVar8 = *(int *)(iVar12 + 0x1c);
    if (iVar8 == 21000) goto LAB_0158bb84;
LAB_0158ba5c:
    if (iVar8 < 0x5209) {
      if (iVar8 == 15000) goto LAB_0158ba84;
      if (15000 < iVar8) {
        if (iVar8 != 16000) {
          if (iVar8 != 20000) goto LAB_0158ba90;
          uVar4 = uVar4 | 0x40000;
        }
        goto LAB_0158ba80;
      }
      if (iVar8 != 12000) {
        if (iVar8 != 13000) goto LAB_0158ba90;
        uVar4 = uVar4 | 0x8000;
      }
      goto LAB_0158ba8c;
    }
    uVar6 = uVar4;
    if (iVar8 == 32000) {
LAB_0158bd1c:
      uVar4 = uVar6 | 0x800000;
LAB_0158bd20:
      uVar4 = uVar4 | 0x400000;
      goto LAB_0158bb84;
    }
    if (iVar8 < 0x7d01) {
      if (iVar8 != 25000) {
        if (iVar8 != 30000) goto LAB_0158ba90;
        uVar4 = uVar4 | 0x800000;
      }
      goto LAB_0158bd20;
    }
    if ((iVar8 == 40000) || (iVar8 == 42000)) {
      uVar6 = uVar4 | 0x1000000;
      param_3[1] = uVar6;
      if (((*(ushort *)(iVar12 + 0x2a4) & 0xf800) != 0) &&
         (((*(ushort *)(iVar12 + 0x2a4) & 0xf800) != 0x800 &&
          (iVar9 = iVar9 + (uint)((int)param_2 < 0 && (param_2 & 0x1f) != 0),
          (1 << (param_2 + iVar9 * -0x20 & 0x3f) &
          *(uint *)(*(int *)(*(int *)(iVar3 + 0x1583968) + iVar7) + (iVar9 + 0x4d4) * 4 + 0x10)) !=
          0)))) {
        uVar6 = uVar4 | 0x3000000;
      }
      goto LAB_0158bd1c;
    }
  }
LAB_0158ba90:
  param_3[1] = uVar4 | 0x800;
  param_3[2] = 7;
  param_3[3] = 0x20;
  uVar5 = *(undefined4 *)(*(int *)(*(int *)(iVar11 + iVar7) + iVar2) + 0x1f8);
  param_3[5] = 4;
  param_3[6] = 1;
  param_3[4] = uVar5;
LAB_0158bac8:
  iVar7 = FUN_01393e80(0xa00fe04);
  if (iVar7 != 0) {
    FUN_013949c0(*(undefined4 *)(iVar3 + 0x158396c),0xa00fe04,*(undefined4 *)(iVar3 + 0x1583970),
                 0x1e8e,*(undefined4 *)(iVar3 + 0x1583974),param_1,param_1,param_2,param_3[1]);
  }
  return 0;
}



// ================================================
// FUN_01594d90 @ 0x1594d90  (entry 0x01594d90)
// ================================================

int FUN_01594d90(int param_1,int param_2,undefined2 *param_3)

{
  undefined2 uVar1;
  int iVar2;
  undefined4 uVar3;
  int iVar4;
  int iVar5;
  uint uVar6;
  int iVar7;
  int iVar8;
  int iVar9;
  
  iVar5 = 4;
  iVar8 = *(int *)(*(int *)(*(int *)(DAT_01594d8c + 0x158cda0) + param_1 * 4) + param_2 * 4);
  if (*(int *)(iVar8 + 0x1c4) != 0xc) {
    iVar5 = 2;
    if ((*(char *)(iVar8 + 0x18) != '\x02') && (iVar5 = 1, *(char *)(iVar8 + 0x18) == '\x05')) {
      iVar5 = 2;
    }
  }
  iVar4 = *(int *)(DAT_01594d8c + 0x158cdc8);
  iVar9 = 0;
  do {
    uVar1 = *param_3;
    iVar7 = (uint)*(byte *)(iVar8 + 0x17) + iVar9;
    uVar6 = (*(byte *)(param_3 + 3) & 0xf) << 4 | (*(byte *)((int)param_3 + 5) & 0xf) << 8 |
            (*(byte *)(param_3 + 2) & 7) << 0xc;
    if (iVar7 == 0) {
      iVar2 = FUN_0158fbdc(param_1,iVar8,0,0x8067,uVar6,0x7ff0);
      if (iVar2 < 0) {
        return iVar2;
      }
      if ((*(char *)(iVar8 + 0x18) == '\x02') || (*(char *)(iVar8 + 0x18) == '\x05')) {
        iVar2 = FUN_0158fbdc(param_1,iVar8,0,0x8077,uVar6,0x7ff0);
        goto joined_r0x01594e4c;
      }
    }
    else {
      if (iVar7 == 1) {
        uVar3 = 0x8077;
      }
      else if (iVar7 == 2) {
        iVar2 = FUN_0158fbdc(param_1,iVar8,0,0x8087,uVar6,0x7ff0);
        if (iVar2 < 0) {
          return iVar2;
        }
        if ((*(char *)(iVar8 + 0x18) != '\x02') && (*(char *)(iVar8 + 0x18) != '\x05'))
        goto LAB_01594e50;
        uVar3 = 0x8097;
      }
      else {
        uVar3 = 0x8097;
      }
      iVar2 = FUN_0158fbdc(param_1,iVar8,0,uVar3,uVar6,0x7ff0);
joined_r0x01594e4c:
      if (iVar2 < 0) {
        return iVar2;
      }
    }
LAB_01594e50:
    iVar7 = FUN_0158f010(param_1,iVar8,*(undefined4 *)(iVar4 + iVar7 * 4 + 200),0x82e2,uVar1);
    if (iVar7 < 0) {
      return iVar7;
    }
    iVar9 = iVar9 + 1;
    param_3 = param_3 + 4;
    if (iVar5 <= iVar9) {
      return 0;
    }
  } while( true );
}



// ================================================
// FUN_01597f00 @ 0x1597f00  (entry 0x01597f00)
// ================================================

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


