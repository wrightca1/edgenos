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
   18  * \file     example_l2_firewall.c
   19  *
   20  * \brief    Example code for Layer2 firewall
   21  *
   22  * \details  This example demonstrates a simple Layer 2 Firewall use case,
   23  * where a network administrator can block unauthorized stations from accessing
   24  * the network. The match criteria for the firewall rules can be either source
   25  * MAC address or destination MAC address. Frames matching the firewall rule
   26  * can either be dropped or forwarded. A centralized firewall server can
   27  * maintain the firewall rules for all the Layer2 switches in the network.
   28  * If the match criteria is source MAC address, packets originating with that
   29  * MAC address are matched. If the match criteria is destination MAC address,
   30  * packets destined to that MAC address are matched.
   31  *
   32  * This application also supports Warmboot feature. Warmboot is the
   33  * process of restarting the device driver software while the hardware is
   34  * running without interrupting the dataplane.
   35  *
   36  * Setup the following envinonment variable before running the application.
   37  * For Cold boot mode, use "export OPENNSL_BOOT_FLAGS = 0x000000".
   38  * For Warm boot mode, use "export OPENNSL_BOOT_FLAGS = 0x200000".
   39  *
   40  ************************************************************************/
   41 
   42 #include <stdio.h>
   43 #include <stdlib.h>
   44 #include <string.h>
   45 #include <sal/driver.h>
   46 #include <opennsl/error.h>
   47 #include <opennsl/l2.h>
   48 #include <opennsl/switch.h>
   49 #include <opennsl/vlan.h>
   50 #include <examples/util.h>
   51 
   52 #define DEFAULT_UNIT  0
   53 #define DEFAULT_VLAN  1
   54 #define MAX_DIGITS_IN_CHOICE 5
   55 
   56 int max_l2_count = 0;  
   57 char example_usage[] =
   58 "Syntax: example_l2_firewall                                           \n\r"
   59 "                                                                      \n\r"
   60 "Paramaters: None.                                                     \n\r"
   61 "                                                                      \n\r"
   62 "Example: The following command is used to allow or block station A    \n\r"
   63 "and station D that are part of VLAN 1 with MAC addresses              \n\r"
   64 "00:00:00:00:00:01 and 00:00:00:00:00:04 respectively.                 \n\r"
   65 "         example_l2_firewall                                          \n\r"
   66 "                                                                      \n\r"
   67 "Usage Guidelines: None.                                               \n\r";
   68 
   69 /*****************************************************************/
   78 int example_l2_firewall_block_station(int unit, opennsl_mac_t mac,
   79     opennsl_vlan_t vlan)
   80 {
   81   opennsl_error_t rv;
   82   opennsl_l2_addr_t addr;
   83 
   84   opennsl_l2_addr_t_init(&addr, mac, vlan);
   85 
   86   /* Set the static flag, discard flags */
   87   addr.flags = (OPENNSL_L2_STATIC | OPENNSL_L2_DISCARD_SRC |
   88       OPENNSL_L2_DISCARD_DST);
   89 
   90   rv = opennsl_l2_addr_add(unit, &addr);
   91 
   92   return rv;
   93 }
   94 /* __doxy_func_body_end__ */
   95 
   96 /*****************************************************************/
  105 int example_l2_firewall_allow_station(int unit, opennsl_mac_t mac,
  106     opennsl_vlan_t vlan)
  107 {
  108   opennsl_error_t rv;
  109   opennsl_l2_addr_t addr;
  110 
  111   /* Check whether the station exists in the firewall rules */
  112   rv = opennsl_l2_addr_get(unit, mac, vlan, &addr);
  113 
  114   if (rv == OPENNSL_E_NONE) {
  115     rv = opennsl_l2_addr_delete(unit, mac, vlan);
  116   }
  117 
  118   return rv;
  119 }
  120 /* __doxy_func_body_end__ */
  121 
  122 /*****************************************************************/
  131 int _opennsl_l2_traverse_cb(
  132     int unit,
  133     opennsl_l2_addr_t *info,
  134     void *user_data)
  135 {
  136   printf("MAC=%02x:%02x:%02x:%02x:%02x:%02x VLAN=%d PORT=%d\n", info->mac[0],info->mac[1],info->mac[2],info->mac[3],info->mac[4],info->mac[5], info->vid, info->port);
  137   max_l2_count++;
  138   return OPENNSL_E_NONE;
  139 }
  140 /* __doxy_func_body_end__ */
  141 
  142 /*****************************************************************/
  149 int main(int argc, char *argv[])
  150 {
  151   opennsl_error_t   rv;
  152   opennsl_vlan_t vlan = 1;
  153   int choice;
  154   int unit = DEFAULT_UNIT;
  155   void *user_data = NULL;
  156   unsigned int warm_boot;
  157   int index = 0;
  158   int age_seconds = 0;  
  159 
  160   if(strcmp(argv[0], "gdb") == 0)
  161   {
  162     index = 1;
  163   }
  164 
  165   /* Example MAC address of stations A, B, C and D */
  166   opennsl_mac_t station_a = {0x00, 0x00, 0x00, 0x00, 0x00, 0x1};
  167   opennsl_mac_t station_d = {0x00, 0x00, 0x00, 0x00, 0x00, 0x4};
  168 
  169   if((argc != (index + 1)) || ((argc > (index + 1)) && (strcmp(argv[index + 1], "--help") == 0))) {
  170     printf("%s\n\r", example_usage);
  171     return OPENNSL_E_PARAM;
  172   }
  173 
  174   /* Initialize the system. */
  175   printf("Initializing the system.\r\n");
  176   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  177 
  178   if(rv != OPENNSL_E_NONE) {
  179     printf("\r\nFailed to initialize the system. Error %s\r\n",
  180         opennsl_errmsg(rv));
  181     return rv;
  182   }
  183 
  184   warm_boot = opennsl_driver_boot_flags_get() & OPENNSL_BOOT_F_WARM_BOOT;
  185 
  186   if (!warm_boot)
  187   {
  188     /* cold boot initialization commands */
  189     rv = example_port_default_config(unit);
  190     if (rv != OPENNSL_E_NONE) {
  191       printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  192              rv, opennsl_errmsg(rv));
  193     }
  194 
  195 
  196     /* Add ports to default vlan. */
  197     printf("Adding ports to default vlan.\r\n");
  198     rv = example_switch_default_vlan_config(unit);
  199     if(rv != OPENNSL_E_NONE) {
  200       printf("\r\nFailed to add default ports. rv: %s\r\n", opennsl_errmsg(rv));
  201       return rv;
  202     }
  203   }
  204 
  205   while (1) {
  206     printf("\r\nUser menu: Select one of the following options\r\n");
  207     printf("1. Apply firewall\n");
  208     printf("2. Remove firewall\n");
  209     printf("3. Show L2 table\n");
  210     printf("4. Save the configuration to scache\n");
  211     printf("5. Set L2 age interval\n");
  212 #ifdef INCLUDE_DIAG_SHELL
  213     printf("9. Launch diagnostic shell\n");
  214 #endif
  215     printf("0. Quit the application\n");
  216 
  217     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  218     {
  219         printf("Invalid option entered. Please re-enter.\n");
  220         continue;
  221     }
  222     switch(choice){
  223 
  224       case 1:
  225       {
  226         /* Apply firewall rules to block station A and D */
  227         rv = example_l2_firewall_block_station(unit, station_a, vlan);
  228         if (rv == OPENNSL_E_NONE) {
  229           printf("L2 firewall: station A blocked successfully\n");
  230         } else {
  231           printf("L2 firewall: failed to apply rule. Error %s\n",
  232                  opennsl_errmsg(rv));
  233           return rv;
  234         }
  235 
  236         rv = example_l2_firewall_block_station(unit, station_d, vlan);
  237         if (rv == OPENNSL_E_NONE) {
  238           printf("L2 firewall rule: station D blocked successfully\n");
  239         } else {
  240           printf("L2 firewall: failed to apply rule. Error %s\n",
  241                  opennsl_errmsg(rv));
  242           return rv;
  243         }
  244         break;
  245       } /* End of case 1 */
  246 
  247       case 2:
  248       {
  249         /* Remove the firewall rules applied to station A and D */
  250         rv = example_l2_firewall_allow_station(unit, station_a, vlan);
  251         if ((rv == OPENNSL_E_NONE) || (rv == OPENNSL_E_NOT_FOUND)) {
  252           printf("L2 firewall: station A un-blocked successfully\n");
  253         } else {
  254           printf("L2 firewall: failed to apply rule. Error %s\n",
  255                  opennsl_errmsg(rv));
  256           return rv;
  257         }
  258 
  259         rv = example_l2_firewall_allow_station(unit, station_d, vlan);
  260         if ((rv == OPENNSL_E_NONE) || (rv == OPENNSL_E_NOT_FOUND)) {
  261           printf("L2 firewall rule: station D un-blocked successfully\n");
  262         }  else {
  263           printf("L2 firewall: failed to apply rule. Error %s\n",
  264                  opennsl_errmsg(rv));
  265           return rv;
  266         }
  267         break;
  268       } /* End of case 2 */
  269 
  270       case 3:
  271       {
  272         /* Iterate over all valid entries in the L2 table */
  273         max_l2_count = 0;
  274         rv = opennsl_l2_traverse(unit, &_opennsl_l2_traverse_cb, user_data);
  275         if(rv != OPENNSL_E_NONE) {
  276           printf("\r\nFailed to iterate over L2 table. rv: %s\r\n", opennsl_errmsg(rv));
  277         }
  278      
  279         printf("\nTotal number of L2 entries: %d\n", max_l2_count);
  280         break;
  281       } /* End of case 3 */
  282 
  283       case 4:
  284       {
  285         /* Sync the current configuration */
  286         rv = opennsl_switch_control_set(DEFAULT_UNIT, opennslSwitchControlSync, 1);
  287         if(rv != OPENNSL_E_NONE) {
  288           printf("Failed to synchronize the configuration to scache. "
  289               "Error %s\n", opennsl_errmsg(rv));
  290           return rv;
  291         }
  292         printf("Warmboot configuration is saved successfully.\n");
  293         break;
  294       } /* End of case 4 */
  295 
  296       case 5:
  297       {
  298         rv = opennsl_l2_age_timer_get(DEFAULT_UNIT, &age_seconds);
  299         if(rv != OPENNSL_E_NONE) {
  300           printf("Failed to get the l2 age interval. "
  301               "Error %s\n", opennsl_errmsg(rv));
  302           return rv;
  303         }
  304         printf("Current L2 age interval: %d secs\n", age_seconds);
  305 
  306         age_seconds = 30;
  307         rv = opennsl_l2_age_timer_set(DEFAULT_UNIT, age_seconds);
  308         if(rv != OPENNSL_E_NONE) {
  309           printf("Failed to set the l2 age interval. "
  310               "Error %s\n", opennsl_errmsg(rv));
  311           return rv;
  312         }
  313         printf("New L2 age interval: %d secs\n", age_seconds);
  314 
  315         break;
  316       } /* End of case 5 */
  317 
  318 #ifdef INCLUDE_DIAG_SHELL
  319       case 9:
  320       {
  321         opennsl_driver_shell();
  322         break;
  323       }
  324 #endif
  325 
  326       case 0:
  327       {
  328         printf("Exiting the application.\n");
  329         rv = opennsl_driver_exit();
  330         return rv;
  331       }
  332       default:
  333         break;
  334     } /* End of switch */
  335   } /* End of while */
  336 
  337   return OPENNSL_E_NONE;
  338 }
  339 /* __doxy_func_body_end__ */
  340 
