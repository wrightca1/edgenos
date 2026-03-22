
uint FUN_0175e26c(int param_1,int param_2,int param_3)

{
  bool bVar1;
  int iVar2;
  int iVar3;
  undefined4 uVar4;
  undefined4 uVar5;
  undefined4 uVar6;
  int iVar7;
  uint uVar8;
  int iVar9;
  int iVar10;
  int iVar11;
  int iVar12;
  undefined4 uVar13;
  undefined4 uVar14;
  undefined4 uVar15;
  int iVar16;
  undefined4 uVar17;
  int iVar18;
  undefined4 uVar19;
  int iVar20;
  int iVar21;
  int iVar22;
  int iVar23;
  byte in_cr0;
  byte in_cr1;
  byte unaff_cr2;
  byte unaff_cr3;
  byte unaff_cr4;
  byte in_cr5;
  byte in_cr6;
  byte in_cr7;
  uint local_f8;
  undefined1 auStack_f4 [124];
  undefined4 local_78;
  undefined4 local_74;
  int local_70;
  int local_6c;
  int local_68;
  undefined4 local_64;
  int local_60;
  int local_5c;
  uint local_4c;
  
  iVar2 = DAT_0175e268;
  local_4c = (uint)(in_cr0 & 0xf) << 0x1c | (uint)(in_cr1 & 0xf) << 0x18 |
             (uint)(unaff_cr2 & 0xf) << 0x14 | (uint)(unaff_cr3 & 0xf) << 0x10 |
             (uint)(unaff_cr4 & 0xf) << 0xc | (uint)(in_cr5 & 0xf) << 8 | (uint)(in_cr6 & 0xf) << 4
             | (uint)(in_cr7 & 0xf);
  iVar21 = param_1 * 4;
  iVar20 = *(int *)(DAT_0175e268 + 0x175627c);
  iVar23 = *(int *)(*(int *)(iVar20 + iVar21) + param_2 * 4);
  local_f8 = 0;
  iVar22 = iVar23 + 0x4d0;
  if ((*(uint *)(iVar23 + 0x544) & 0x4400) != 0) {
    FUN_013949c0(*(undefined4 *)(DAT_0175e268 + 0x17567d0),
                 *(int *)(DAT_0175e268 + 0x1756328) + 0x448,param_1,param_2,param_3,
                 *(undefined4 *)(iVar23 + 0x1c0),*(undefined4 *)(iVar23 + 0x1c4),
                 *(undefined4 *)(iVar23 + 0x4fc));
  }
  local_74 = *(undefined4 *)(iVar23 + 0x4e0);
  uVar14 = *(undefined4 *)(iVar23 + 0x55c);
  uVar15 = *(undefined4 *)(iVar23 + 0x4e8);
  if (param_3 == 0) {
    if ((*(uint *)(iVar23 + 0x100) & 0x20) != 0) {
      *(uint *)(iVar23 + 0x100) = *(uint *)(iVar23 + 0x100) ^ 0x20;
    }
    if (*(int *)(iVar23 + 0x4fc) == 0) {
      *(undefined4 *)(iVar23 + 0x4e0) = 0xf;
    }
    if ((*(int *)(iVar23 + 0x468) == 0) && (*(int *)(iVar23 + 0x4f4) != 6)) {
      uVar5 = *(undefined4 *)(iVar2 + 0x17562e4);
      *(undefined4 *)(iVar23 + 0x4f0) = 0x12;
      FUN_0177b524(uVar5,iVar22,&local_f8);
    }
    uVar5 = *(undefined4 *)(iVar2 + 0x1756660);
    *(undefined4 *)(iVar23 + 0x4f0) = 0x20000;
    FUN_0177b524(uVar5,iVar22,&local_f8);
    uVar5 = FUN_0177b018(uVar15);
    *(undefined4 *)(iVar23 + 0x4e0) = uVar5;
    *(undefined4 *)(iVar23 + 0x4e8) = uVar15;
    *(undefined4 *)(iVar23 + 0x55c) = 0;
    if ((*(uint *)(iVar23 + 0x51c) >> 0xc & 0xf) == 2) {
      iVar21 = *(int *)(iVar23 + 0x520);
      if ((*(int *)(iVar23 + 0x230) < 0xa034) || (1 < iVar21 - 2U)) goto LAB_0175e460;
      if (*(int *)(iVar23 + 0x1c4) - 4U < 2) {
        local_f8 = FUN_0174e344(iVar22,4,1);
        goto LAB_0175e45c;
      }
      *(undefined4 *)(iVar23 + 0x524) = 0;
LAB_0175e5b0:
      uVar5 = *(undefined4 *)(iVar2 + 0x1756380);
      *(undefined4 *)(iVar23 + 0x520) = 0;
      FUN_0177b524(uVar5,iVar22,&local_f8);
    }
    else {
LAB_0175e45c:
      iVar21 = *(int *)(iVar23 + 0x520);
LAB_0175e460:
      *(undefined4 *)(iVar23 + 0x524) = 0;
      if (iVar21 != 0) goto LAB_0175e5b0;
    }
    iVar21 = *(int *)(iVar23 + 0x1c0);
    if ((((iVar21 == 5) || (iVar21 == 0x102)) || (iVar21 == 0x104)) || (iVar21 == 0x114)) {
      *(undefined4 *)(iVar23 + 0x4b8) = 0;
      *(undefined1 *)(iVar23 + 0x4bd) = 0;
    }
    goto LAB_0175e4a0;
  }
  *(undefined2 *)(iVar23 + 0x4c4) = 0;
  *(undefined2 *)(iVar23 + 0x4c2) = 0;
  *(undefined2 *)(iVar23 + 0x4c0) = 0;
  *(undefined2 *)(iVar23 + 0x4c8) = 0;
  *(undefined4 *)(iVar23 + 0x4b8) = 0;
  *(undefined1 *)(iVar23 + 0x4bd) = 0;
  if ((*(uint *)(iVar23 + 0x548) & 0x2000000) != 0) {
    *(uint *)(iVar23 + 0x548) = *(uint *)(iVar23 + 0x548) ^ 0x2000000;
  }
  iVar7 = *(int *)(iVar23 + 0x1c0);
  if (iVar7 == 0) {
    uVar8 = *(uint *)(iVar23 + 0x1c4);
    if (uVar8 == 0) {
      if (*(int *)(iVar23 + 0x23c) == 0) {
        if (*(int *)(iVar23 + 0x1a0) == 0) {
          local_78 = 0;
          iVar11 = 6;
          local_68 = 0x28;
          local_70 = 6;
        }
        else {
          local_78 = 0;
          if (*(int *)(iVar23 + 0x244) == 0x42) {
            iVar11 = 3;
            local_68 = 0x42;
            local_70 = 3;
          }
          else {
            iVar11 = 2;
            local_68 = 0x28;
            local_70 = 2;
          }
        }
      }
      else {
        local_78 = 0;
        local_68 = 0x28;
        iVar11 = 5;
        local_70 = 5;
      }
    }
    else {
      if (uVar8 < 7) {
                    /* WARNING: Could not emulate address calculation at 0x0175f4e0 */
                    /* WARNING: Treating indirect jump as call */
        uVar8 = (*(code *)(*(int *)(*(int *)(iVar2 + 0x17567d4) + uVar8 * 4) +
                          *(int *)(iVar2 + 0x17567d4)))();
        return uVar8;
      }
      local_68 = 0x28;
LAB_0175e394:
      iVar11 = 0;
      FUN_013949c0(*(undefined4 *)(iVar2 + 0x17567f8),param_1,param_2,param_3,iVar7,uVar8);
      local_78 = 0;
      local_70 = 0;
    }
  }
  else if (iVar7 == 6) {
    local_78 = 1;
    iVar11 = 1;
    local_68 = 0x46;
    local_70 = 1;
  }
  else if (iVar7 < 7) {
    if (iVar7 == 2) {
LAB_0175f4b8:
      local_78 = 0;
      iVar11 = 1;
      local_68 = 0x42;
      local_70 = 1;
    }
    else if (iVar7 < 3) {
      if (iVar7 != 1) {
LAB_0175e388:
        uVar8 = *(uint *)(iVar23 + 0x1c4);
        local_68 = 0x42;
        goto LAB_0175e394;
      }
      local_78 = 0;
      iVar11 = 4;
      local_68 = 0x42;
      local_70 = 4;
    }
    else {
      if (iVar7 != 4) {
        if (iVar7 == 5) goto LAB_0175f4b8;
        goto LAB_0175e388;
      }
LAB_0175f5b4:
      local_78 = 0;
      iVar11 = 7;
      local_68 = 0x42;
      local_70 = 7;
    }
  }
  else {
    if (iVar7 == 0x102) goto LAB_0175f4b8;
    if (iVar7 < 0x103) {
      if (iVar7 == 8) goto LAB_0175f5b4;
      if (iVar7 != 0x14) goto LAB_0175e388;
LAB_0175e62c:
      local_78 = 0;
      iVar11 = 7;
      local_68 = 0x42;
      *(uint *)(iVar23 + 0x548) = *(uint *)(iVar23 + 0x548) | 0x2000000;
      local_70 = 7;
    }
    else {
      if (iVar7 != 0x104) {
        if (iVar7 == 0x114) goto LAB_0175e62c;
        goto LAB_0175e388;
      }
      local_78 = 0;
      iVar11 = 7;
      local_68 = 0x42;
      local_70 = 7;
    }
  }
  bVar1 = false;
  iVar7 = 0;
  if (*(int *)(iVar23 + 0x520) != iVar11) {
    if ((*(uint *)(iVar23 + 0x544) & 0x40000) != 0) {
      FUN_013949c0(*(undefined4 *)(iVar2 + 0x17567d8),*(int *)(iVar2 + 0x1756328) + 0x448,param_1,
                   param_2,*(undefined4 *)
                            (*(int *)(iVar2 + 0x17562fc) + *(int *)(iVar23 + 0x520) * 4),
                   *(undefined4 *)(*(int *)(iVar2 + 0x17562fc) + local_70 * 4));
    }
    iVar16 = *(int *)(iVar2 + 0x1756358);
    iVar9 = *(int *)(iVar16 + iVar21);
    uVar8 = *(uint *)(iVar9 + 0x68a8);
    if (((int)uVar8 < 0) || (*(int *)(iVar9 + 0x68ac) < (int)uVar8)) {
      bVar1 = false;
      iVar7 = 1;
    }
    else {
      iVar18 = uVar8 << 2;
      iVar12 = *(int *)(iVar2 + 0x1756294);
      bVar1 = false;
      iVar7 = 0;
      iVar3 = *(int *)(iVar2 + 0x1756328) + 0x448;
      local_6c = 0;
      do {
        iVar10 = ((int)uVar8 >> 5) + (uint)((int)uVar8 < 0 && (uVar8 & 0x1f) != 0);
        if (((1 << (uVar8 + iVar10 * -0x20 & 0x3f) & *(uint *)(iVar9 + (iVar10 + 0x1a28) * 4 + 0x10)
             ) != 0) && (iVar9 = *(int *)(*(int *)(iVar20 + iVar21) + iVar18), iVar9 != 0)) {
          if ((*(uint *)(iVar23 + 0x544) & 0x40000) != 0) {
            local_5c = iVar3;
            FUN_013949c0(*(undefined4 *)(iVar2 + 0x17567dc),*(undefined4 *)(iVar2 + 0x17567fc),
                         param_1,uVar8,*(uint *)(iVar9 + 0x100) >> 2 & 1,
                         *(uint *)(iVar9 + 0x100) >> 5 & 1,*(undefined2 *)(iVar9 + 0x1a));
            iVar3 = local_5c;
          }
          if (((*(int *)(iVar9 + 0x108) != 0) && (*(int *)(iVar9 + 0x108) == iVar12)) &&
             (*(short *)(iVar9 + 0x1a) == *(short *)(iVar23 + 0x1a))) {
            if ((*(int *)(iVar9 + 0x558) != local_68) &&
               (bVar1 = true, (*(uint *)(iVar23 + 0x544) & 0x40000) != 0)) {
              local_5c = iVar3;
              FUN_013949c0(*(undefined4 *)(iVar2 + 0x17567e0),*(undefined4 *)(iVar2 + 0x17567fc),
                           param_1,uVar8,*(int *)(iVar9 + 0x558),*(undefined4 *)(iVar23 + 0x558),
                           local_68);
              iVar3 = local_5c;
            }
            if ((*(uint *)(iVar9 + 0x100) & 0x24) == 0x24) {
              local_5c = iVar3;
              iVar3 = FUN_017514b0(*(undefined4 *)(iVar9 + 0x520),local_70);
              if (iVar3 != 0) {
                *(int *)(iVar9 + 0x520) = iVar11;
                iVar7 = 1;
              }
              local_6c = 1;
              iVar3 = local_5c;
              if ((*(uint *)(iVar23 + 0x544) & 0x40000) != 0) {
                FUN_013949c0(*(undefined4 *)(iVar2 + 0x17567e4),local_5c,param_1,uVar8,param_2,
                             *(undefined4 *)
                              (*(int *)(iVar2 + 0x17562fc) + *(int *)(iVar9 + 0x520) * 4),iVar7);
                iVar3 = local_5c;
              }
            }
          }
        }
        uVar8 = uVar8 + 1;
        if ((int)uVar8 < 0) break;
        iVar9 = *(int *)(iVar16 + iVar21);
        iVar18 = iVar18 + 4;
      } while ((int)uVar8 <= *(int *)(iVar9 + 0x68ac));
      if (local_6c == 0) {
        iVar7 = 1;
      }
    }
  }
  iVar9 = *(int *)(iVar23 + 0x4fc);
  *(int *)(iVar23 + 0x520) = iVar11;
  *(undefined4 *)(iVar23 + 0x524) = local_78;
  if (iVar9 == 0) {
LAB_0175eac0:
    *(undefined4 *)(iVar23 + 0x4e0) = 0xf;
LAB_0175eac8:
    bVar1 = iVar7 == 0;
    if (bVar1) {
      local_78 = *(undefined4 *)(iVar2 + 0x17562ec);
      uVar17 = *(undefined4 *)(iVar2 + 0x17562e4);
      uVar13 = *(undefined4 *)(iVar2 + 0x17562e8);
      uVar5 = *(undefined4 *)(iVar2 + 0x17562f0);
    }
    else {
      iVar7 = FUN_01755ee8(param_1,*(undefined2 *)(iVar23 + 0x1a));
      *(uint *)(iVar23 + 0x100) = *(uint *)(iVar23 + 0x100) | 0x20;
      if (iVar7 == 0) {
        uVar17 = *(undefined4 *)(iVar2 + 0x17562e4);
        *(undefined4 *)(iVar23 + 0x4f0) = 0x109;
        FUN_0177b524(uVar17,iVar22,&local_f8);
        local_78 = *(undefined4 *)(iVar2 + 0x17562ec);
        *(undefined4 *)(iVar23 + 0x4f0) = 0;
        FUN_0177b524(local_78,iVar22,&local_f8);
        uVar13 = *(undefined4 *)(iVar2 + 0x17562e8);
        *(undefined4 *)(iVar23 + 0x4f0) = 0x10;
        FUN_0177b524(uVar13,iVar22,&local_f8);
        *(undefined4 *)(iVar23 + 0x4f0) = 0x20;
        FUN_0177b524(uVar13,iVar22,&local_f8);
        FUN_0174e344(iVar22,2,0);
        uVar5 = *(undefined4 *)(iVar2 + 0x17562f0);
        if ((*(uint *)(iVar23 + 0x548) & 0x80000) != 0) {
          *(undefined4 *)(iVar23 + 0x4f0) = 2;
          FUN_0177b524(uVar5,iVar22,&local_f8);
        }
        *(undefined4 *)(iVar23 + 0x4f0) = 0;
        FUN_0177b524(uVar5,iVar22,&local_f8);
        *(undefined4 *)(iVar23 + 0x4f0) = 0x106;
        FUN_0177b524(uVar17,iVar22,&local_f8);
        *(undefined4 *)(iVar23 + 0x4f0) = 0x102;
        FUN_0177b524(uVar17,iVar22,&local_f8);
        FUN_0137fa34(1000);
        uVar6 = FUN_0177b018(uVar15);
        *(undefined4 *)(iVar23 + 0x55c) = 0;
        *(undefined4 *)(iVar23 + 0x4e0) = uVar6;
        uVar6 = *(undefined4 *)(iVar2 + 0x1756524);
        *(undefined4 *)(iVar23 + 0x4f0) = 0;
        FUN_0177b524(uVar6,iVar22,&local_f8);
        FUN_0137fa34(2000);
        if (*(int *)(iVar23 + 0x4fc) == 0) {
          *(undefined4 *)(iVar23 + 0x4e0) = 0xf;
        }
        *(undefined4 *)(iVar23 + 0x55c) = uVar14;
        if ((*(uint *)(iVar23 + 0x544) & 0x40000) != 0) {
          FUN_013949c0(*(undefined4 *)(iVar2 + 0x17567ec),*(int *)(iVar2 + 0x1756328) + 0x448,
                       param_1,param_2,*(undefined4 *)(iVar23 + 0x558),local_68);
        }
        uVar6 = *(undefined4 *)(iVar2 + 0x1756510);
        *(undefined4 *)(iVar23 + 0x4f0) = 1;
        FUN_0177b524(uVar6,iVar22,&local_f8);
        *(uint *)(iVar23 + 0x100) = *(uint *)(iVar23 + 0x100) | 0x20;
        if (4000 < *(uint *)(iVar23 + 0x43c)) {
          FUN_0137fa34(*(uint *)(iVar23 + 0x43c) - 4000);
        }
        iVar11 = *(int *)(iVar2 + 0x1756358);
        iVar7 = *(int *)(iVar11 + iVar21);
        uVar8 = *(uint *)(iVar7 + 0x68a8);
        if ((-1 < (int)uVar8) && ((int)uVar8 <= *(int *)(iVar7 + 0x68ac))) {
          iVar9 = uVar8 << 2;
          iVar16 = *(int *)(iVar2 + 0x1756294);
          uVar6 = *(undefined4 *)(iVar2 + 0x17567f0);
          iVar18 = *(int *)(iVar2 + 0x1756328) + 0x448;
          do {
            iVar3 = ((int)uVar8 >> 5) + (uint)((int)uVar8 < 0 && (uVar8 & 0x1f) != 0);
            if (((((1 << (uVar8 + iVar3 * -0x20 & 0x3f) &
                   *(uint *)(iVar7 + (iVar3 + 0x1a28) * 4 + 0x10)) != 0) &&
                 (iVar7 = *(int *)(*(int *)(iVar20 + iVar21) + iVar9), iVar7 != 0)) &&
                (*(int *)(iVar7 + 0x108) != 0)) &&
               ((*(int *)(iVar7 + 0x108) == iVar16 &&
                (*(short *)(iVar7 + 0x1a) == *(short *)(iVar23 + 0x1a))))) {
              if ((*(uint *)(iVar23 + 0x544) & 0x40000) != 0) {
                local_64 = uVar6;
                local_60 = iVar18;
                local_5c = iVar16;
                FUN_013949c0(uVar6,iVar18,param_1,uVar8,param_2,*(undefined4 *)(iVar23 + 0x558));
                iVar16 = local_5c;
                uVar6 = local_64;
                iVar18 = local_60;
              }
              *(undefined4 *)(iVar7 + 0x558) = *(undefined4 *)(iVar23 + 0x558);
            }
            uVar8 = uVar8 + 1;
            if ((int)uVar8 < 0) break;
            iVar7 = *(int *)(iVar11 + iVar21);
            iVar9 = iVar9 + 4;
          } while ((int)uVar8 <= *(int *)(iVar7 + 0x68ac));
        }
        uVar6 = FUN_0177b018(uVar15);
        *(undefined4 *)(iVar23 + 0x55c) = 0;
        *(undefined4 *)(iVar23 + 0x4f0) = 1;
        *(undefined4 *)(iVar23 + 0x4e0) = uVar6;
        FUN_0177b524(*(undefined4 *)(iVar2 + 0x1756524),iVar22,&local_f8);
        FUN_0177b524(*(undefined4 *)(iVar2 + 0x175654c),iVar22,&local_f8);
        if (*(int *)(iVar23 + 0x4fc) == 0) {
          *(undefined4 *)(iVar23 + 0x4e0) = 0xf;
        }
        *(undefined4 *)(iVar23 + 0x55c) = uVar14;
        uVar8 = FUN_0174f4a0(param_1,param_2,auStack_f4,0xd);
        if ((int)uVar8 < 0) {
          return uVar8;
        }
        uVar8 = FUN_0175308c(param_1,param_2,auStack_f4);
        if ((int)uVar8 < 0) {
          return uVar8;
        }
        *(undefined4 *)(iVar23 + 0x4f0) = 6;
        bVar1 = false;
        FUN_0177b524(uVar17,iVar22,&local_f8);
        FUN_0137fa34(1000);
        iVar9 = *(int *)(iVar23 + 0x4fc);
      }
      else {
        local_78 = *(undefined4 *)(iVar2 + 0x17562ec);
        iVar9 = *(int *)(iVar23 + 0x4fc);
        bVar1 = true;
        uVar17 = *(undefined4 *)(iVar2 + 0x17562e4);
        uVar13 = *(undefined4 *)(iVar2 + 0x17562e8);
        uVar5 = *(undefined4 *)(iVar2 + 0x17562f0);
      }
    }
    if (iVar9 != 0) goto LAB_0175e804;
    *(undefined4 *)(iVar23 + 0x4e0) = 0xf;
    *(undefined4 *)(iVar23 + 0x4f0) = 0x10e;
    FUN_0177b524(uVar17,iVar22,&local_f8);
    uVar8 = *(uint *)(iVar23 + 0x548);
  }
  else {
    if (bVar1) goto LAB_0175eac8;
    if ((*(uint *)(iVar23 + 0x544) & 0x40000) != 0) {
      FUN_013949c0(*(undefined4 *)(iVar2 + 0x17567e8),*(int *)(iVar2 + 0x1756328) + 0x448,param_1,
                   param_2,*(undefined4 *)(iVar23 + 0x558),local_68);
    }
    uVar5 = *(undefined4 *)(iVar2 + 0x1756510);
    *(undefined4 *)(iVar23 + 0x4f0) = 3;
    FUN_0177b524(uVar5,iVar22,&local_f8);
    if (*(int *)(iVar23 + 0x4fc) == 0) {
      iVar7 = 0;
      iVar9 = 0;
      goto LAB_0175eac0;
    }
    local_78 = *(undefined4 *)(iVar2 + 0x17562ec);
    uVar17 = *(undefined4 *)(iVar2 + 0x17562e4);
    bVar1 = true;
    uVar13 = *(undefined4 *)(iVar2 + 0x17562e8);
    uVar5 = *(undefined4 *)(iVar2 + 0x17562f0);
LAB_0175e804:
    *(undefined4 *)(iVar23 + 0x4f0) = 0x10e;
    FUN_0177b524(uVar17,iVar22,&local_f8);
    uVar8 = *(uint *)(iVar23 + 0x548);
  }
  if ((uVar8 & 0x400000) != 0) {
    *(undefined4 *)(iVar23 + 0x4f0) = 0x10f;
    FUN_0177b524(uVar17,iVar22,&local_f8);
  }
  *(undefined4 *)(iVar23 + 0x4f0) = 0x105;
  FUN_0177b524(uVar17,iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 0x109;
  FUN_0177b524(uVar17,iVar22,&local_f8);
  if ((*(uint *)(iVar23 + 0x51c) >> 8 & 0xf | *(uint *)(iVar23 + 0x51c) & 0xf0) < 2) {
    iVar7 = 4;
    iVar20 = 0;
    iVar21 = iVar20;
    if (*(int *)(iVar23 + 0x4fc) != 0) {
      if (*(int *)(iVar23 + 0x4fc) == 2) {
        iVar7 = 2;
        if ((*(uint *)(iVar23 + 0x55c) & 3) == 2) {
          iVar7 = 4;
          iVar20 = 2;
          iVar21 = iVar20;
        }
      }
      else {
        iVar20 = *(int *)(iVar23 + 0x4e8);
        iVar7 = iVar20 + 1;
        iVar21 = iVar20;
      }
    }
    do {
      iVar11 = iVar20 + 1;
      FUN_0175df90(param_1,param_2,iVar20,0);
      iVar20 = iVar11;
    } while (iVar11 < iVar7);
    if (*(int *)(iVar23 + 0x4fc) == 0) goto LAB_0175e89c;
LAB_0175ef28:
    uVar6 = FUN_0174dde0(param_1,param_2,uVar15);
    *(undefined4 *)(iVar23 + 0x4e0) = uVar6;
  }
  else {
    *(undefined4 *)(iVar23 + 0x4f0) = 0x114;
    iVar7 = 0;
    iVar21 = 0;
    FUN_0177b524(uVar17,iVar22,&local_f8);
    if (*(int *)(iVar23 + 0x4fc) != 0) goto LAB_0175ef28;
LAB_0175e89c:
    *(undefined4 *)(iVar23 + 0x4e0) = 0xf;
  }
  uVar6 = *(undefined4 *)(iVar2 + 0x1756520);
  *(undefined4 *)(iVar23 + 0x4f0) = 0;
  FUN_0177b524(uVar6,iVar22,&local_f8);
  uVar6 = *(undefined4 *)(iVar2 + 0x1756660);
  *(undefined4 *)(iVar23 + 0x4f0) = 0x10001;
  FUN_0177b524(uVar6,iVar22,&local_f8);
  if (*(int *)(iVar23 + 0x4fc) == 0) {
    *(undefined4 *)(iVar23 + 0x4e0) = 0xf;
  }
  *(undefined4 *)(iVar23 + 0x4f0) = 0;
  FUN_0177b524(local_78,iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 0x10;
  FUN_0177b524(uVar13,iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 0x20;
  FUN_0177b524(uVar13,iVar22,&local_f8);
  FUN_0174e344(iVar22,2,0);
  if ((*(uint *)(iVar23 + 0x548) & 0x80000) != 0) {
    *(undefined4 *)(iVar23 + 0x4f0) = 2;
    FUN_0177b524(uVar5,iVar22,&local_f8);
  }
  *(undefined4 *)(iVar23 + 0x4f0) = 0;
  FUN_0177b524(uVar5,iVar22,&local_f8);
  uVar19 = *(undefined4 *)(iVar2 + 0x1756300);
  *(undefined4 *)(iVar23 + 0x4f0) = 1;
  FUN_0177b524(uVar19,iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 0x100;
  FUN_0177b524(uVar19,iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 8;
  FUN_0177b524(uVar19,iVar22,&local_f8);
  if (bVar1) {
    uVar8 = FUN_0174f4a0(param_1,param_2,auStack_f4,0xd);
    if ((int)uVar8 < 0) {
      return uVar8;
    }
    uVar8 = FUN_0175308c(param_1,param_2,auStack_f4);
    if ((int)uVar8 < 0) {
      return uVar8;
    }
  }
  *(undefined4 *)(iVar23 + 0x4f0) = 10;
  FUN_0177b524(uVar17,iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 0xb;
  FUN_0177b524(uVar17,iVar22,&local_f8);
  if ((*(int *)(iVar23 + 0x520) == 0) || (*(int *)(iVar23 + 0x1c8) == 0)) {
    uVar4 = 0x20000;
  }
  else {
    uVar4 = 0x20001;
  }
  *(undefined4 *)(iVar23 + 0x4f0) = uVar4;
  FUN_0177b524(uVar6,iVar22,&local_f8);
  iVar20 = *(int *)(iVar23 + 0x1c0);
  if ((((iVar20 == 5) || (iVar20 == 0x102)) || (local_70 == 3)) ||
     ((iVar20 == 0x114 || (iVar20 == 0x104)))) {
    *(uint *)(iVar23 + 0x4f0) = (uint)**(ushort **)(iVar2 + 0x17567f4) << 8 | 0x20;
  }
  else {
    uVar8 = *(uint *)(iVar23 + 0x51c);
    if (((uVar8 >> 8 & 0xf | uVar8 & 0xf0) < 2) && ((uVar8 >> 0xc & 0xf) != 2)) {
      *(uint *)(iVar23 + 0x4f0) = (uint)**(ushort **)(iVar2 + 0x175652c) << 8 | 0x20;
    }
    else {
      *(uint *)(iVar23 + 0x4f0) = (uint)**(ushort **)(iVar2 + 0x1756530) << 8 | 0x20;
    }
  }
  FUN_0177b524(*(undefined4 *)(iVar2 + 0x1756528),iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 0x103;
  FUN_0177b524(uVar17,iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 0x102;
  FUN_0177b524(uVar17,iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 0x10c;
  FUN_0177b524(uVar17,iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 1;
  FUN_0177b524(uVar19,iVar22,&local_f8);
  iVar20 = *(int *)(iVar23 + 0x520);
  if (((iVar20 == 1) || (iVar20 == 4)) || (iVar20 == 7)) {
    if (*(int *)(iVar23 + 0x1c8) == 0) {
      uVar6 = 0x70;
    }
    else {
      uVar6 = *(undefined4 *)(iVar2 + 0x17563bc);
      *(undefined4 *)(iVar23 + 0x4f0) = 0;
      FUN_0177b524(uVar6,iVar22,&local_f8);
      uVar6 = 0x10;
    }
    *(undefined4 *)(iVar23 + 0x4f0) = uVar6;
    FUN_0177b524(uVar19,iVar22,&local_f8);
    *(undefined4 *)(iVar23 + 0x4f0) = 0x11;
    FUN_0177b524(uVar17,iVar22,&local_f8);
    bVar1 = local_70 == 5;
  }
  else {
    bVar1 = local_70 == 5;
    if (bVar1) {
      if (*(int *)(iVar23 + 0x1c8) == 0) {
        if (*(int *)(iVar23 + 0x1d0) < 1) goto LAB_0175f6b4;
LAB_0175f65c:
        uVar6 = 0x30;
      }
      else {
        uVar6 = *(undefined4 *)(iVar2 + 0x17563bc);
        *(undefined4 *)(iVar23 + 0x4f0) = 0;
        FUN_0177b524(uVar6,iVar22,&local_f8);
        if (0 < *(int *)(iVar23 + 0x1d0)) goto LAB_0175f65c;
LAB_0175f6b4:
        uVar6 = 0x10;
      }
      *(undefined4 *)(iVar23 + 0x4f0) = uVar6;
      FUN_0177b524(uVar19,iVar22,&local_f8);
      *(undefined4 *)(iVar23 + 0x4f0) = 0x111;
      FUN_0177b524(uVar17,iVar22,&local_f8);
    }
  }
  if (((*(uint *)(iVar23 + 0x51c) >> 8 & 0xf | *(uint *)(iVar23 + 0x51c) & 0xf0) < 2) &&
     (iVar21 < iVar7)) {
    do {
      iVar20 = iVar21 + 1;
      FUN_0175df90(param_1,param_2,iVar21,2);
      iVar21 = iVar20;
    } while (iVar20 != iVar7);
  }
  if ((*(uint *)(iVar23 + 0x548) & 0x10) == 0) {
    *(undefined4 *)(iVar23 + 0x4f0) = 2;
    FUN_0177b524(uVar17,iVar22,&local_f8);
  }
  FUN_0137fa34(1000);
  if (*(int *)(iVar23 + 0x4fc) == 0) {
    *(undefined4 *)(iVar23 + 0x4e0) = 0xf;
  }
  if (*(int *)(iVar23 + 0x520) == 7) {
    *(undefined4 *)(iVar23 + 0x4f0) = 0x112;
    FUN_0177b524(uVar17,iVar22,&local_f8);
    if (*(int *)(iVar23 + 0x1c0) != 8) goto LAB_0175ec5c;
    uVar6 = 0x113;
  }
  else {
    *(undefined4 *)(iVar23 + 0x4f0) = 0x12;
    FUN_0177b524(uVar17,iVar22,&local_f8);
LAB_0175ec5c:
    uVar6 = 0x13;
  }
  *(undefined4 *)(iVar23 + 0x4f0) = uVar6;
  FUN_0177b524(uVar17,iVar22,&local_f8);
  if ((*(uint *)(iVar23 + 0x548) & 0x10002000) == 0) {
    *(undefined4 *)(iVar23 + 0x4f0) = 9;
    FUN_0177b524(uVar17,iVar22,&local_f8);
  }
  uVar6 = FUN_0177b018(uVar15);
  *(undefined4 *)(iVar23 + 0x4e0) = uVar6;
  *(undefined4 *)(iVar23 + 0x4e8) = uVar15;
  *(undefined4 *)(iVar23 + 0x55c) = 0;
  if (bVar1) {
    uVar8 = 0xb;
  }
  else {
    uVar8 = 0xd;
    if (*(int *)(iVar23 + 0x1b0) == 0) {
      uVar8 = 0;
    }
    if ((*(int *)(iVar23 + 0x1ac) != 0) && (*(int *)(iVar23 + 0x4fc) == 0)) {
      uVar8 = uVar8 | 0xe;
    }
  }
  *(uint *)(iVar23 + 0x4f0) = uVar8;
  FUN_0177b524(*(undefined4 *)(iVar2 + 0x17563d8),iVar22,&local_f8);
  if (((((*(uint *)(iVar23 + 0x51c) >> 0xc & 0xf) == 2) && (0xa033 < *(int *)(iVar23 + 0x230))) &&
      (*(int *)(iVar23 + 0x520) - 2U < 2)) && (*(int *)(iVar23 + 0x1c4) - 4U < 2)) {
    local_f8 = FUN_0174e344(iVar22,4,1);
  }
  FUN_0177b524(*(undefined4 *)(iVar2 + 0x1756380),iVar22,&local_f8);
  if (*(int *)(iVar23 + 0x4fc) == 0) {
    *(undefined4 *)(iVar23 + 0x4e0) = 0xf;
  }
  *(undefined4 *)(iVar23 + 0x55c) = uVar14;
  *(undefined4 *)(iVar23 + 0x4f0) = 1;
  FUN_0177b524(uVar13,iVar22,&local_f8);
  *(undefined4 *)(iVar23 + 0x4f0) = 1;
  FUN_0177b524(local_78,iVar22,&local_f8);
  uVar8 = *(uint *)(iVar23 + 0x548);
  if ((uVar8 & 0x10002000) == 0) {
    *(undefined4 *)(iVar23 + 0x4f0) = 5;
    FUN_0177b524(uVar17,iVar22,&local_f8);
    uVar8 = *(uint *)(iVar23 + 0x548);
  }
  if ((uVar8 & 0x80000) != 0) {
    *(undefined4 *)(iVar23 + 0x4f0) = 2;
    FUN_0177b524(uVar5,iVar22,&local_f8);
  }
  *(undefined4 *)(iVar23 + 0x4f0) = 1;
  FUN_0177b524(uVar5,iVar22,&local_f8);
  if ((*(uint *)(iVar23 + 0x548) & 0x2000) == 0) {
    *(undefined4 *)(iVar23 + 0x4f0) = 0xe;
    FUN_0177b524(uVar17,iVar22,&local_f8);
    if ((*(uint *)(iVar23 + 0x548) & 0x400000) != 0) {
      *(undefined4 *)(iVar23 + 0x4f0) = 0x10f;
      FUN_0177b524(uVar17,iVar22,&local_f8);
    }
  }
  *(undefined4 *)(iVar23 + 0x4f0) = 0x102;
  FUN_0177b524(uVar17,iVar22,&local_f8);
  if ((*(uint *)(iVar23 + 0x548) & 0x10) == 0) {
    *(undefined4 *)(iVar23 + 0x4f0) = 2;
    FUN_0177b524(uVar17,iVar22,&local_f8);
  }
  iVar21 = *(int *)(iVar23 + 0x1c0);
  if ((iVar21 == 5) || (iVar21 == 0x102)) {
    *(undefined4 *)(iVar23 + 0x4b8) = 2;
    iVar21 = FUN_0137ff88();
    *(undefined1 *)(iVar23 + 0x4bd) = 0;
    *(undefined1 *)(iVar23 + 0x4bc) = 0;
    *(int *)(iVar23 + 0x4b4) = iVar21 + 4000000;
  }
  else if ((iVar21 == 0x104) || (iVar21 == 0x114)) {
    *(undefined4 *)(iVar23 + 0x4b8) = 2;
    iVar21 = FUN_0137ff88();
    *(undefined1 *)(iVar23 + 0x4bd) = 0;
    *(undefined1 *)(iVar23 + 0x4bc) = 0;
    *(int *)(iVar23 + 0x4b4) = iVar21 + 6000000;
  }
LAB_0175e4a0:
  *(undefined4 *)(iVar23 + 0x4e8) = uVar15;
  *(undefined4 *)(iVar23 + 0x55c) = uVar14;
  *(undefined4 *)(iVar23 + 0x4e0) = local_74;
  uVar8 = FUN_01756844(param_1,*(undefined4 *)(iVar23 + 0x4d8),2,6);
  return uVar8 | local_f8;
}


