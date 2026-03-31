    1 /*********************************************************************
    2  *
    3  * (C) Copyright Broadcom Corporation 2013-2017
    4  *
    5  *  Licensed under the Apache License, Version 2.0 (the "License");
    6  *  you may not use this file except in compliance with the License.
    7  *  You may obtain a copy of the License at
    8  *
    9  *      http://www.apache.org/licenses/LICENSE-2.0
   10  *
   11  *  Unless required by applicable law or agreed to in writing, software
   12  *  distributed under the License is distributed on an "AS IS" BASIS,
   13  *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   14  *  See the License for the specific language governing permissions and
   15  *  limitations under the License.
   16  *
   17  **********************************************************************
   18  *
   19  * \file         example_field_ip_block.c
   20  *
   21  * \brief        OPENNSL example program to demonstrate field processor
   22  *
   23  * \details      This example demonstrates blocking of traffic from
   24  *               hosts with IP addresses 192.168.199.36 to 192.168.199.47
   25  *               with the exception of 192.168.199.40.
   26  *
   27  **********************************************************************/
   28 #include <stdio.h>
   29 #include <stdlib.h>
   30 #include <string.h>
   31 #include <sal/driver.h>
   32 #include <opennsl/error.h>
   33 #include <opennsl/vlan.h>
   34 #include <opennsl/port.h>
   35 #include <opennsl/field.h>
   36 #include <examples/util.h>
   37 
   38 char example_usage[] =
   39 "Syntax: example_field_ip_block                                         \n\r"
   40 "                                                                       \n\r"
   41 "Paramaters: None.                                                      \n\r"
   42 "                                                                       \n\r"
   43 "Example: The following command is used to block traffic from hosts with\n\r"
   44 "         IP addresses 192.168.199.36 to 192.168.199.47 with the        \n\r"
   45 "         exception of 192.168.199.40. The traffic should be of VLAN 1. \n\r"
   46 "         example_field_ip_block                                        \n\r"
   47 "                                                                       \n\r"
   48 "Usage Guidelines: None.                                                \n\r";
   49 
   50 #define DEFAULT_UNIT  0
   51 #define DEFAULT_VLAN  1
   52 #define MAX_DIGITS_IN_CHOICE 5
   53 
   54 #define ETHERTYPE           0x0800
   55 #define ETHERMASK           0xffff
   56 #define IPADDR_36           0xC0A8C724 /* 192.168.199.36 */
   57 #define IPADDR_39           0xC0A8C727 /* 192.168.199.39 */
   58 #define IPMASK_BLOCK_36_39  0xFFFFFFFC /* Mask: 192.168.199.36-39 */
   59 #define IPADDR_40           0xC0A8C728 /* 192.168.199.40 */
   60 #define IPMASK_BLOCK_40_47  0xFFFFFFF8 /* Mask: 192.168.199.40-47 */
   61 #define IPMASK_HOST_UNBLOCK 0xFFFFFFFF /* Unblock host mask */
   62 #define IPADDR_47           0xC0A8C72F /* 192.168.199.47 */
   63 
   64 #define CALL_IF_ERROR_RETURN(op)                              \
   65   do                                                          \
   66   {                                                           \
   67     int __rv__;                                               \
   68       if ((__rv__ = (op)) < 0) {                              \
   69         printf("%s:%s: line %d rv: %d failed: %s\n",          \
   70             __FILE__, __FUNCTION__, __LINE__, __rv__,         \
   71             opennsl_errmsg(__rv__));                          \
   72       }                                                       \
   73   } while(0)
   74 /* __doxy_func_body_end__ */
   75 
   76 /* debug prints */
   77 int verbose = 3;
   78 
   79 /**************************************************************************/
   86 int example_fp_ip_block(int unit) {
   87   opennsl_field_qset_t qset;
   88   opennsl_field_group_t group;
   89   opennsl_field_entry_t entry;
   90 
   91   OPENNSL_FIELD_QSET_INIT( qset );
   92 
   94   OPENNSL_FIELD_QSET_ADD(qset, opennslFieldQualifyEtherType);
   95   OPENNSL_FIELD_QSET_ADD(qset, opennslFieldQualifySrcIp);
   96 
   98   OPENNSL_IF_ERROR_RETURN(opennsl_field_group_create(unit, qset,
   99         OPENNSL_FIELD_GROUP_PRIO_ANY, &group));
  100 
  101 
  103   {
  105     OPENNSL_IF_ERROR_RETURN(opennsl_field_entry_create(unit, group, &entry));
  106 
  107     opennsl_field_qualify_EtherType(unit, entry, ETHERTYPE, ETHERMASK);
  108     opennsl_field_qualify_SrcIp(unit, entry, IPADDR_40, IPMASK_HOST_UNBLOCK);
  109     opennsl_field_entry_install(unit, entry);
  110 
  112     OPENNSL_IF_ERROR_RETURN(opennsl_field_action_add(unit, entry,
  113           opennslFieldActionDropCancel, 0, 0));
  114 
  115     OPENNSL_IF_ERROR_RETURN(opennsl_field_entry_install(unit, entry));
  116 
  117     if(verbose >= 2) {
  118       printf("Updated Field Processor to unblock host");
  119       print_ip_addr("", IPADDR_40);
  120       printf("\n");
  121     }
  122 
  123   }
  124 
  126   {
  128     OPENNSL_IF_ERROR_RETURN(opennsl_field_entry_create(unit, group, &entry));
  129 
  130     opennsl_field_qualify_EtherType(unit, entry, ETHERTYPE, ETHERMASK);
  131     opennsl_field_qualify_SrcIp(unit, entry, IPADDR_36, IPMASK_BLOCK_36_39);
  132     opennsl_field_entry_install(unit, entry);
  133 
  135     OPENNSL_IF_ERROR_RETURN(opennsl_field_action_add(unit, entry,
  136           opennslFieldActionDrop, 0, 0));
  137 
  138     OPENNSL_IF_ERROR_RETURN(opennsl_field_entry_install(unit, entry));
  139 
  140     if(verbose >= 2) {
  141       printf("Updated Field Processor to block hosts with IP addresses");
  142       print_ip_addr(" from", IPADDR_36);
  143       print_ip_addr(" to", IPADDR_39);
  144       printf("\n");
  145     }
  146   }
  147 
  149   {
  151     OPENNSL_IF_ERROR_RETURN(opennsl_field_entry_create(unit, group, &entry));
  152 
  153     opennsl_field_qualify_EtherType(unit, entry, ETHERTYPE, ETHERMASK);
  154     opennsl_field_qualify_SrcIp(unit, entry, IPADDR_40, IPMASK_BLOCK_40_47);
  155     opennsl_field_entry_install(unit, entry);
  156 
  158     OPENNSL_IF_ERROR_RETURN(opennsl_field_action_add(unit, entry,
  159           opennslFieldActionDrop, 0, 0));
  160 
  161     OPENNSL_IF_ERROR_RETURN(opennsl_field_entry_install(unit, entry));
  162 
  163     if(verbose >= 2) {
  164       printf("Updated Field Processor to block hosts with IP addresses");
  165       print_ip_addr(" from", IPADDR_40);
  166       print_ip_addr(" to", IPADDR_47);
  167       printf("\n");
  168     }
  169 
  170   }
  171 
  172   return OPENNSL_E_NONE;
  173 }
  174 /* __doxy_func_body_end__ */
  175 
  176 /********************************************************************/
  183 int main(int argc, char *argv[])
  184 {
  185   int rv = OPENNSL_E_NONE;
  186   int choice;
  187   int unit = DEFAULT_UNIT;
  188 
  189   if((argc != 1) || ((argc > 1) && (strcmp(argv[1], "--help") == 0))) {
  190     printf("%s\n\r", example_usage);
  191     return OPENNSL_E_PARAM;
  192   }
  193 
  194   /* Initialize the system */
  195   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  196 
  197   if(rv != OPENNSL_E_NONE) {
  198     printf("\r\nFailed to initialize the system.\r\n");
  199     return rv;
  200   }
  201 
  202   /* cold boot initialization commands */
  203   rv = example_port_default_config(unit);
  204   if (rv != OPENNSL_E_NONE) {
  205     printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  206            rv, opennsl_errmsg(rv));
  207   }
  208 
  209 
  210   /* Add ports to default vlan. */
  211   printf("Adding ports to default vlan.\r\n");
  212   rv = example_switch_default_vlan_config(unit);
  213   if(rv != OPENNSL_E_NONE) {
  214     printf("\r\nFailed to add ports to default VLAN. Error: %s\r\n",
  215         opennsl_errmsg(rv));
  216   }
  217 
  218   rv = example_fp_ip_block(unit);
  219 
  220   while(1) {
  221     printf("\r\nUser menu: Select one of the following options\r\n");
  222 #ifdef INCLUDE_DIAG_SHELL
  223     printf("9. Launch diagnostic shell\n");
  224 #endif
  225     printf("0. Quit the application.\r\n");
  226 
  227     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  228     {
  229       printf("Invalid option entered. Please re-enter.\n");
  230       continue;
  231     }
  232     switch(choice)
  233     {
  234 #ifdef INCLUDE_DIAG_SHELL
  235       case 9:
  236       {
  237         opennsl_driver_shell();
  238         break;
  239       }
  240 #endif
  241 
  242       case 0:
  243       {
  244         printf("Exiting the application.\n");
  245         rv = opennsl_driver_exit();
  246         return rv;
  247       }
  248       default:
  249       break;
  250     } /* End of switch */
  251   } /* End of while */
  252 
  253   return rv;
  254 }
  255 /* __doxy_func_body_end__ */
