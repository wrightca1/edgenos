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
   19  * \file         example_field_redirect.c
   20  *
   21  * \brief        OPENNSL example program to demonstrate field processor
   22  *
   23  * \details      This example demonstrates redirection of traffic with 
   24  *               destination MAC = MAC_DA to a given destination port 
   25  *               along with the option to display the number of redirected 
   26  *               packets
   27  *
   28  **********************************************************************/
   29 #include <stdio.h>
   30 #include <stdlib.h>
   31 #include <string.h>
   32 #include <sal/driver.h>
   33 #include <opennsl/error.h>
   34 #include <opennsl/vlan.h>
   35 #include <opennsl/port.h>
   36 #include <opennsl/field.h>
   37 #include <examples/util.h>
   38 
   39 char example_usage[] =
   40 "Syntax: example_field_redirect                                         \n\r"
   41 "                                                                       \n\r"
   42 "Paramaters: None.                                                      \n\r"
   43 "                                                                       \n\r"
   44 "Example: The following command is used to redirect traffic with        \n\r"
   45 "         destination MAC = MAC_DA to a specified destination port.     \n\r"
   46 "         It also provides an option to display the number of packets   \n\r"
   47 "         redirected to destination port.                               \n\r"
   48 "                                                                       \n\r"
   49 "         example_field_redirect                                        \n\r"
   50 "                                                                       \n\r"
   51 "Usage Guidelines: None.                                                \n\r";
   52 
   53 #define DEFAULT_UNIT  0
   54 #define NEWCOS_VAL    3
   55 #define MAC_DA        {0x00, 0x00, 0x01, 0x00, 0x02, 0x00}
   56 /* __doxy_func_body_end__ */
   57 #define MAC_MASK      {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}
   58 /* __doxy_func_body_end__ */
   59 #define DEFAULT_STAT_ID 2
   60 
   61 opennsl_field_group_t grp  = 1;
   62 opennsl_field_entry_t eid  = 0;
   63 int stat_id                = DEFAULT_STAT_ID;
   64 
   65 opennsl_field_stat_t stats = opennslFieldStatPackets;
   66 
   67 #define CALL_IF_ERROR_RETURN(op)                              \
   68   do                                                          \
   69   {                                                           \
   70     int __rv__;                                               \
   71       if ((__rv__ = (op)) < 0) {                              \
   72         printf("%s:%s: line %d rv: %d failed: %s\n",          \
   73             __FILE__, __FUNCTION__, __LINE__, __rv__,         \
   74             opennsl_errmsg(__rv__));                          \
   75       }                                                       \
   76   } while(0)
   77 /* __doxy_func_body_end__ */
   78 
   79 /**************************************************************************/
   88 int example_fp_redirect(int unit, int dport)
   89 {
   90   int rv = OPENNSL_E_NONE;
   91   opennsl_field_qset_t qset;
   92   opennsl_field_aset_t aset;
   93   int pri = OPENNSL_FIELD_GROUP_PRIO_ANY;
   94   opennsl_mac_t bpdu_mac_da = MAC_DA;
   95   opennsl_mac_t mac_mask = MAC_MASK;
   96   int dest_gport;
   97   int cos = NEWCOS_VAL;
   98 
   99   int                  old_stat_id;
  100   unsigned int         counter_proc;
  101   unsigned int         counter_id;
  102 
  104   OPENNSL_FIELD_QSET_INIT(qset);
  105   OPENNSL_FIELD_QSET_ADD(qset, opennslFieldQualifyDstMac);
  106 
  108   OPENNSL_FIELD_ASET_INIT(aset);
  109   OPENNSL_FIELD_ASET_ADD(aset, opennslFieldActionStat);
  110   OPENNSL_FIELD_ASET_ADD(aset, opennslFieldActionRedirectPort);
  111   OPENNSL_FIELD_ASET_ADD(aset, opennslFieldActionPrioIntNew);
  112 
  114   CALL_IF_ERROR_RETURN(opennsl_field_group_create_id(unit, qset, pri, grp));
  115 
  116   CALL_IF_ERROR_RETURN(opennsl_field_group_action_set(unit, grp, aset));
  117 
  122   CALL_IF_ERROR_RETURN(opennsl_field_entry_create_id(unit, grp, eid));
  123   CALL_IF_ERROR_RETURN(opennsl_field_qualify_DstMac(unit, eid,
  124         bpdu_mac_da, mac_mask));
  125 
  126   stat_id = DEFAULT_STAT_ID;
  127   CALL_IF_ERROR_RETURN(opennsl_field_stat_create_id(unit, grp, 1,
  128         &stats, stat_id));
  129 
  130   /* Create a counter for core 1 as well */
  131   /* QumranMx has 16 counter processors. We map these processors for
  132    * use with ingress field processor. By default, we first create
  133    * counter for core 0. Below we create an equivalent counter for core 1.
  134    * This code will need to change in the new SDK, because the counter processor
  135    * will be 4 bits as opposed to 2 bits
  136    */
  137   old_stat_id = stat_id;
  138   counter_proc = OPENNSL_FIELD_STAT_ID_PROCESSOR_GET(stat_id);
  139   counter_id = OPENNSL_FIELD_STAT_ID_COUNTER_GET(stat_id);
  140   OPENNSL_FIELD_STAT_ID_SET(stat_id, counter_proc + 1, counter_id);
  141 
  142   CALL_IF_ERROR_RETURN(opennsl_field_stat_create_id(unit, grp, 1, &stats, stat_id));
  143 
  144   /* Restore the old stat id */
  145   stat_id = old_stat_id;
  146 
  147 
  149   CALL_IF_ERROR_RETURN(opennsl_field_action_add(unit, eid,
  150         opennslFieldActionStat, stat_id, 0));
  151   OPENNSL_GPORT_LOCAL_SET(dest_gport, dport);
  152   CALL_IF_ERROR_RETURN(opennsl_field_action_add(unit, eid,
  153         opennslFieldActionRedirectPort, 0, dest_gport));
  154   CALL_IF_ERROR_RETURN(opennsl_field_action_add(unit, eid,
  155         opennslFieldActionPrioIntNew, cos, 0));
  156 
  157   rv = opennsl_field_group_install(unit, grp);
  158   if (OPENNSL_E_NONE != rv)
  159   {
  160     printf("Failed to install the group, Error %s\n", opennsl_errmsg(rv));
  161     CALL_IF_ERROR_RETURN(opennsl_field_group_destroy(unit, grp));
  162     return rv;
  163   }
  164 
  165   printf("Packet qualification criteria: \n");
  166   l2_print_mac(" ==> MAC DA -", bpdu_mac_da);
  167   printf("\n");
  168   printf("Configured system to count and redirect qualified traffic to "
  169       "port %d", dport);
  170 
  171   return rv;
  172 }
  173 /* __doxy_func_body_end__ */
  174 
  175 /********************************************************************/
  183 int main(int argc, char *argv[])
  184 {
  185   int rv = OPENNSL_E_NONE;
  186   int choice;
  187   int unit = DEFAULT_UNIT;
  188   uint64 val;
  189   uint64 val_1;
  190   int stat_id_1;
  191   int port;
  192   unsigned int counter_proc;
  193   unsigned int counter_id;
  194 
  195   if((argc != 1) || ((argc > 1) && (strcmp(argv[1], "--help") == 0))) {
  196     printf("%s\n\r", example_usage);
  197     return OPENNSL_E_PARAM;
  198   }
  199 
  200   /* Initialize the system */
  201   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  202 
  203   if(rv != OPENNSL_E_NONE) {
  204     printf("\r\nFailed to initialize the system.\r\n");
  205     return rv;
  206   }
  207 
  208   rv = example_port_default_config(unit);
  209   if (rv != OPENNSL_E_NONE) {
  210     printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  211            rv, opennsl_errmsg(rv));
  212   }
  213 
  214   /* Add ports to default vlan. */
  215   printf("Adding ports to default vlan.\r\n");
  216   rv = example_switch_default_vlan_config(unit);
  217   if(rv != OPENNSL_E_NONE) {
  218     printf("\r\nFailed to add ports to default VLAN. Error: %s\r\n",
  219         opennsl_errmsg(rv));
  220   }
  221 
  222   while(1) {
  223     printf("\r\nUser menu: Select one of the following options\r\n");
  224     printf("1. Apply filter to redirect traffic\n");
  225     printf("2. Remove redirection filter\n");
  226     printf("3. Display the number of packets redirected\n");
  227 #ifdef INCLUDE_DIAG_SHELL
  228     printf("9. Launch diagnostic shell\n");
  229 #endif
  230     printf("0. Quit the application.\r\n");
  231 
  232     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  233     {
  234       printf("Invalid option entered. Please re-enter.\n");
  235       continue;
  236     }
  237     switch(choice)
  238     {
  239       case 1:
  240       {
  241         printf("Enter port number to which traffic needs to be redirected:\n");
  242         if(example_read_user_choice(&port) != OPENNSL_E_NONE)
  243         {
  244           printf("Invalid option entered. Please re-enter.\n");
  245           continue;
  246         }
  247         rv = example_fp_redirect(unit, port);
  248         if (rv != OPENNSL_E_NONE) {
  249           printf("Failed to configure the system for traffic redirection, "
  250               "Error %s\n", opennsl_errmsg(rv));
  251           return rv;
  252         }
  253         break;
  254       }
  255 
  256       case 2:
  257       {
  258         /* Destroy any attached stat_id before destroying the entry */
  259         rv = opennsl_field_entry_stat_get(unit, eid, &stat_id);
  260         if (rv == OPENNSL_E_NONE)
  261         {
  262           counter_proc = OPENNSL_FIELD_STAT_ID_PROCESSOR_GET(stat_id);
  263           counter_id = OPENNSL_FIELD_STAT_ID_COUNTER_GET(stat_id);
  264 
  265           opennsl_field_entry_stat_detach(unit, eid, stat_id);
  266 
  267           /* stat_id deletion might fail if the stat_id is being shared */
  268           (void) opennsl_field_stat_destroy(unit, stat_id);
  269 
  270           /* Delete Core 1 counter as well, we create identical counters on both cores */
  271           OPENNSL_FIELD_STAT_ID_SET(stat_id, counter_proc + 1, counter_id);
  272           (void) opennsl_field_stat_destroy(unit, stat_id);
  273         }
  274 
  275         CALL_IF_ERROR_RETURN(opennsl_field_entry_destroy(unit, eid));
  276         CALL_IF_ERROR_RETURN(opennsl_field_group_destroy(unit, grp));
  277         printf("Removed the redirection filter\n");
  278         break;
  279       }
  280 
  281       case 3:
  282       {
  283         rv = opennsl_field_stat_get(unit, stat_id, stats, &val);
  284         if (rv != OPENNSL_E_NONE) {
  285           printf("Failed to get the statistics for core 0, "
  286               "Error %s\n", opennsl_errmsg(rv));
  287           return rv;
  288         }
  289 
  290         /* Get core 1 statistics */
  291         stat_id_1 = stat_id | (1 << 29);
  292         rv = opennsl_field_stat_get(unit, stat_id_1, stats, &val_1);
  293         if (rv != OPENNSL_E_NONE) {
  294           printf("Failed to get the statistics for core 1, "
  295               "Error %s\n", opennsl_errmsg(rv));
  296           return rv;
  297         }
  298         val += val_1;
  299         printf("Number of packets redirected: %lld (0x%0llx)\n", val, val);
  300 
  301         break;
  302       }
  303 
  304 #ifdef INCLUDE_DIAG_SHELL
  305       case 9:
  306       {
  307         opennsl_driver_shell();
  308         break;
  309       }
  310 #endif
  311 
  312       case 0:
  313       {
  314         printf("Exiting the application.\n");
  315         rv = opennsl_driver_exit();
  316         return rv;
  317       }
  318       default:
  319       break;
  320     } /* End of switch */
  321   } /* End of while */
  322 
  323   return rv;
  324 }
  325 /* __doxy_func_body_end__ */
