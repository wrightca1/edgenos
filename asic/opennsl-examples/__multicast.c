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
   19  * \file         example_l2_multicast.c
   20  *
   21  * \brief        OpenNSL example application for Layer 2 multicast
   22  *
   23  * \details      This application allows the user to
   24  *               1) Create multicast group
   25  *               2) Add a port to multicast group
   26  *               3) Remove a port from multicast group
   27  *
   28  **********************************************************************/
   29 #include <stdio.h>
   30 #include <stdlib.h>
   31 #include <string.h>
   32 #include <sal/driver.h>
   33 #include <opennsl/error.h>
   34 #include <opennsl/l2.h>
   35 #include <opennsl/multicast.h>
   36 #include <examples/util.h>
   37 
   38 #define DEFAULT_UNIT 0
   39 #define DEFAULT_VLAN 1
   40 #define MCAST_GROUP_ID  10
   41 
   42 char example_usage[] =
   43 "Syntax: example_l2_multicast                                          \n\r"
   44 "                                                                      \n\r"
   45 "Paramaters: None                                                      \n\r"
   46 "                                                                      \n\r"
   47 "Example: The following command is used to create a multicast group    \n\r"
   48 "         and add or delete multicast members.                         \n\r"
   49 "         example_l2_multicast                                         \n\r"
   50 "                                                                      \n\r"
   51 "Usage Guidelines: This program request the user to enter the following\n\r"
   52 "                  parameters interactively                            \n\r"
   53 "       MAC address - Multicast group MAC address                      \n\r"
   54 "       port        - Multicast group port members                     \n\r"
   55 "                                                                      \n\r"
   56 "       Testing:                                                       \n\r"
   57 "       1) Add ports to the multicast group and verify that traffic    \n\r"
   58 "          destined to Multicast MAC address is received by all        \n\r"
   59 "          multicast group members.                                    \n\r"
   60 "       2) Remove a port from the multicast group and verify that      \n\r"
   61 "          traffic destined to Multicast MAC address is not received   \n\r"
   62 "          by removed multicast group member.                          \n\r"
   63 "       3) Verify that traffic to unknown unicast MAC address is       \n\r"
   64 "          received by all ports in the VLAN.                          \n\r"
   65 "                                                                      \n\r";
   66 
   67 /**************************************************************************/
   79 int example_l2_entry_add(int unit, opennsl_mac_t mac, opennsl_vlan_t vlan,
   80     int is_mc, int dest_gport, int dest_mcg)
   81 {
   82   int rv = OPENNSL_E_NONE;
   83   opennsl_l2_addr_t l2_addr;
   84 
   85   opennsl_l2_addr_t_init(&l2_addr, mac, vlan);
   86   if(is_mc == 1) {
   87     /* add multicast entry */
   88     l2_addr.flags = OPENNSL_L2_STATIC | OPENNSL_L2_MCAST;
   89     l2_addr.l2mc_group = dest_mcg;
   90   } else {
   91     /* add static entry */
   92     l2_addr.flags = OPENNSL_L2_STATIC;
   93     l2_addr.port = dest_gport;
   94   }
   95   rv = opennsl_l2_addr_add(unit,&l2_addr);
   96   return rv;
   97 }
   98 /* __doxy_func_body_end__ */
   99 
  100 /**************************************************************************/
  109 int example_multicast_create(int unit, int mcg, int egress_mc)
  110 {
  111   int rv = OPENNSL_E_NONE;
  112   int flags;
  113   opennsl_multicast_t mc_group = mcg;
  114 
  115   flags = OPENNSL_MULTICAST_WITH_ID | OPENNSL_MULTICAST_TYPE_L2;
  116   if (egress_mc)
  117   {
  118     flags |= OPENNSL_MULTICAST_EGRESS_GROUP;
  119   }
  120   else
  121   {
  122     flags |= OPENNSL_MULTICAST_INGRESS_GROUP;
  123   }
  124   rv = opennsl_multicast_create(unit, flags, &mc_group);
  125   return rv;
  126 }
  127 /* __doxy_func_body_end__ */
  128 
  129 /*****************************************************************/
  136 int main(int argc, char *argv[])
  137 {
  138   int rv = 0;
  139   int choice;
  140   int unit = DEFAULT_UNIT;
  141   int port;
  142   int mcg = MCAST_GROUP_ID;
  143   int encap_id = NULL;
  144   opennsl_gport_t gport;
  145   char macstr[18];
  146   opennsl_mac_t mac;
  147   opennsl_vlan_t vlan = DEFAULT_VLAN;
  148 
  149   if((argc != 1) || ((argc > 1) && (strcmp(argv[1], "--help") == 0))) {
  150     printf("%s\n\r", example_usage);
  151     return OPENNSL_E_PARAM;
  152   }
  153 
  154   /* Initialize the system. */
  155   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  156 
  157   if(rv != OPENNSL_E_NONE) {
  158     printf("\r\nFailed to initialize the system. Error: %s\r\n",
  159            opennsl_errmsg(rv));
  160     return rv;
  161   }
  162 
  163   rv = example_port_default_config(unit);
  164   if (rv != OPENNSL_E_NONE) {
  165     printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  166            rv, opennsl_errmsg(rv));
  167   }
  168 
  169   /* Add ports to default vlan. */
  170   printf("Adding ports to default vlan.\r\n");
  171   rv = example_switch_default_vlan_config(unit);
  172   if(rv != OPENNSL_E_NONE) {
  173     printf("\r\nFailed to add default ports. Error: %s\r\n",
  174         opennsl_errmsg(rv));
  175   }
  176 
  177   if(example_multicast_create(unit, mcg, 0) != OPENNSL_E_NONE)
  178   {
  179     printf("\r\nFailed to create multicast group. Error: %s\r\n",
  180         opennsl_errmsg(rv));
  181     return OPENNSL_E_FAIL;
  182   }
  183 
  184   printf("Enter multicast MAC address in xx:xx:xx:xx:xx:xx format "
  185       "(Ex: 01:00:00:01:02:03): \n");
  186   if(example_read_user_string(macstr, sizeof(macstr)) == NULL)
  187   {
  188     printf("\n\rInvalid MAC address entered.\n\r");
  189     return OPENNSL_E_FAIL;
  190   }
  191 
  192   if(opennsl_mac_parse(macstr, mac) != OPENNSL_E_NONE)
  193   {
  194     printf("\n\rFailed to parse input MAC address.\n\r");
  195     return OPENNSL_E_FAIL;
  196   }
  197 
  198   rv = example_l2_entry_add(unit, mac, vlan, 1, 0, mcg);
  199   if(rv != OPENNSL_E_NONE)
  200   {
  201     printf("\n\rFailed to update FDB with multicast MAC address. "
  202         "Error: %s\n\r", opennsl_errmsg(rv));
  203     return OPENNSL_E_FAIL;
  204   }
  205 
  206   while(1) {
  207     printf("\r\nUser menu: Select one of the following options\r\n");
  208     printf("1. Add port to multicast group\n");
  209     printf("2. Remove port from multicast group\n");
  210 #ifdef INCLUDE_DIAG_SHELL
  211     printf("9. Launch diagnostic shell\n");
  212 #endif
  213     printf("0. Quit the application.\r\n");
  214 
  215     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  216     {
  217       printf("Invalid option entered. Please re-enter.\n");
  218       continue;
  219     }
  220 
  221     switch(choice)
  222     {
  223       case 1:
  224       {
  225         printf("Enter multicast group member port number: \n");
  226         if(example_read_user_choice(&port) != OPENNSL_E_NONE)
  227         {
  228           printf("Invalid option entered. Please re-enter.\n");
  229           continue;
  230         }
  231 
  232         if(encap_id == NULL)
  233         {
  234           rv = opennsl_multicast_l2_encap_get(0, mcg, 3, vlan, &encap_id);
  235           if(rv != OPENNSL_E_NONE)
  236           {
  237             printf("Failed to get encapsulation ID, Error: %d\r\n",
  238                 opennsl_errmsg(rv));
  239             return rv;
  240           }
  241         }
  242         OPENNSL_GPORT_LOCAL_SET(gport, port);
  243         rv = opennsl_multicast_ingress_add(0, mcg, gport, encap_id);
  244         if(rv != OPENNSL_E_NONE && rv != OPENNSL_E_EXISTS)
  245         {
  246           printf("\r\nFailed to add the port to multicast group. "
  247               "Error: %s\r\n", opennsl_errmsg(rv));
  248         }
  249         else
  250         {
  251           printf("Port %d is added to Multicast group successfully\n", port);
  252         }
  253         break;
  254       }
  255       case 2:
  256       {
  257         printf("Enter multicast group member port number: \n");
  258         if(example_read_user_choice(&port) != OPENNSL_E_NONE)
  259         {
  260           printf("Invalid option entered. Please re-enter.\n");
  261           continue;
  262         }
  263 
  264         OPENNSL_GPORT_LOCAL_SET(gport, port);
  265         rv = opennsl_multicast_ingress_delete(0, mcg, gport, encap_id);
  266         if(rv != OPENNSL_E_NONE && rv != OPENNSL_E_NOT_FOUND)
  267         {
  268           printf("\r\nFailed to remove the port from multicast group. "
  269               "Error: %s\r\n", opennsl_errmsg(rv));
  270         }
  271         else
  272         {
  273           printf("Port %d is removed from Multicast group "
  274               "successfully\n", port);
  275         }
  276         break;
  277       }
  278 #ifdef INCLUDE_DIAG_SHELL
  279       case 9:
  280       {
  281         opennsl_driver_shell();
  282         break;
  283       }
  284 #endif
  285 
  286       case 0:
  287       {
  288         printf("Exiting the application.\n");
  289         rv = opennsl_driver_exit();
  290         return rv;
  291       }
  292       default:
  293       break;
  294     } /* End of switch */
  295   }
  296 
  297   return rv;
  298 }
  299 /* __doxy_func_body_end__ */
  300 
