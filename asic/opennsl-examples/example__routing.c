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
   19  * \file         example_routing.c
   20  *
   21  * \brief        OPENNSL example program to demonstrate routing
   22  *
   23  * \details      This example demonstrates routing of packets across
   24  * two different VLAN's by using host route and default routes.
   25  * In the first case, switch installs a host route to route
   26  * packets from INGRESS_VLAN, destination MAC = MY_MAC to HOST1
   27  * attached to EGRESS_VLAN. But, in the second case a default
   28  * route is installed to route packets from INGRESS_VLAN,
   29  * destination MAC = MY_MAC destined to DEFAULT_SUBNET_IP to
   30  * egress port that is a member of EGRESS_VLAN.
   31  *
   32  * This application also supports Warmboot feature. Warmboot is
   33  * the process of restarting the device driver software while
   34  * the hardware is running without interrupting the dataplane.
   35  *
   36  * Setup the following envinonment variable before running the application.
   37  * For Cold boot mode, use "export OPENNSL_BOOT_FLAGS = 0x000000".
   38  * For Warm boot mode, use "export OPENNSL_BOOT_FLAGS = 0x200000".
   39  *
   40  **********************************************************************/
   41 #include <stdio.h>
   42 #include <stdlib.h>
   43 #include <string.h>
   44 #include <sal/driver.h>
   45 #include <opennsl/error.h>
   46 #include <opennsl/vlan.h>
   47 #include <opennsl/switch.h>
   48 #include <opennsl/l2.h>
   49 #include <opennsl/l3.h>
   50 #include <examples/util.h>
   51 
   52 char example_usage[] =
   53 "Syntax: example_routing                                                \n\r"
   54 "                                                                       \n\r"
   55 "Paramaters:                                                            \n\r"
   56 "       in_sysport  - Ingress port number on which test packet is       \n\r"
   57 "                     received                                          \n\r"
   58 "       out_sysport - Egress port number to which packet is routed.     \n\r"
   59 "                                                                       \n\r"
   60 "Example: The following command is used to demonstrate routing from     \n\r"
   61 "         port 2 to port 6.                                             \n\r"
   62 "         example_routing 2 6                                           \n\r"
   63 "                                                                       \n\r"
   64 "Usage Guidelines: This program request the user to enter the           \n\r"
   65 "       ingress and egress port numbers. Using the host route, it routes\n\r"
   66 "       the packets arrived on ingress port with VLAN = 10,             \n\r"
   67 "       destination MAC = 00:11:22:33:99:58 to host 20.1.1.2 attached   \n\r"
   68 "       to egress port that is a member of VLAN 20. Using the default   \n\r"
   69 "       route, it also routes packets with VLAN = 10, destination       \n\r"
   70 "       MAC = 00:11:22:33:99:58 destinated to subnet 55.0.0.0 to egress \n\r"
   71 "       port that is a member of VLAN 20.                               \n\r"
   72 "       The routed packet should have destination MAC same as           \n\r"
   73 "       00:00:70:5B:C7:34 with TTL decremented by 1.                    \n\r";
   74 
   75 
   76 #define INGRESS_VLAN    10
   77 #define EGRESS_VLAN     20
   78 #define HOST1           0x14010102         /* 20.1.1.2 */
   79 #define DEFAULT_SUBNET_IP   0x37000000     /* 55.0.0.0 */
   80 #define DEFAULT_SUBNET_MASK 0xff000000     /* /8 network */
   81 #define MY_MAC          {0x00, 0x11, 0x22, 0x33, 0x99, 0x58}
   82 /* __doxy_func_body_end__ */
   83 #define NEXTHOP_MAC     {0x00, 0x00, 0x70, 0x5B, 0xC7, 0x34}
   84 /* __doxy_func_body_end__ */
   85 #define MAX_DIGITS_IN_CHOICE 5
   86 
   87 /* debug prints */
   88 int verbose = 3;
   89 
   90 /**************************************************************************/
  101 int example_l2_addr_add(int unit, opennsl_mac_t mac, int port, int vid) {
  102 
  103   int rv;
  104   opennsl_l2_addr_t l2addr;
  105 
  106   opennsl_l2_addr_t_init(&l2addr, mac, vid);
  107   l2addr.port  = port;
  108   l2addr.flags = (OPENNSL_L2_L3LOOKUP | OPENNSL_L2_STATIC);
  109 
  110   rv = opennsl_l2_addr_add(unit, &l2addr);
  111   if (rv != OPENNSL_E_NONE) {
  112     return rv;
  113   }
  114 
  115   return OPENNSL_E_NONE;
  116 }
  117 /* __doxy_func_body_end__ */
  118 
  119 /**************************************************************************/
  131 int example_vlan_port_add(int unit, int open_vlan,
  132     opennsl_vlan_t vlan, int port)
  133 {
  134   opennsl_pbmp_t  pbmp, upbmp;
  135   int rc = OPENNSL_E_NONE;
  136 
  137   if(open_vlan) {
  138     rc = opennsl_vlan_create(unit, vlan);
  139     if (rc != OPENNSL_E_NONE) {
  140       printf("failed to create vlan(%d). Return code %s",
  141           vlan, opennsl_errmsg(rc));
  142       return rc;
  143     }
  144     if(verbose >= 3) {
  145       printf("Created VLAN: %d\n", vlan);
  146     }
  147   }
  148 
  149   /* Add the test ports to the vlan */
  150   OPENNSL_PBMP_CLEAR(pbmp);
  151   OPENNSL_PBMP_CLEAR(upbmp);
  152   OPENNSL_PBMP_PORT_ADD(pbmp, port);
  153   rc = opennsl_vlan_port_add(unit, vlan, pbmp, upbmp);
  154   if (rc != OPENNSL_E_NONE && rc != OPENNSL_E_EXISTS) {
  155     return rc;
  156   }
  157   return rc;
  158 }
  159 /* __doxy_func_body_end__ */
  160 
  161 /**************************************************************************/
  179 int example_create_l3_intf(int unit, int open_vlan, opennsl_gport_t port,
  180     opennsl_vlan_t vid, opennsl_mac_t mac_addr, int *l3_id) {
  181 
  182   int rc;
  183   opennsl_l3_intf_t l3_intf;
  184 
  185   /* Create VLAN and add port to the VLAN membership */
  186   rc = example_vlan_port_add(unit, open_vlan, vid, port);
  187   if (rc != OPENNSL_E_NONE && rc != OPENNSL_E_EXISTS) {
  188     printf("Fail to add port %d to vlan %d. Return Code: %s\n", port, vid, opennsl_errmsg(rc));
  189     return rc;
  190   }
  191   if(verbose >= 2) {
  192     printf("Port %d is participating in VLAN: %d\n", port, vid);
  193   }
  194 
  195   /* Create L3 interface */
  196   opennsl_l3_intf_t_init(&l3_intf);
  197   memcpy(l3_intf.l3a_mac_addr, mac_addr, 6);
  198   l3_intf.l3a_vid = vid;
  199   rc = opennsl_l3_intf_create(unit, &l3_intf);
  200   if (rc != OPENNSL_E_NONE) {
  201     printf("l3_setup: opennsl_l3_intf_create failed: %s\n", opennsl_errmsg(rc));
  202     return rc;
  203   }
  204 
  205   *l3_id = l3_intf.l3a_intf_id;
  206 
  207   if(verbose >= 3) {
  208     printf("L3 interface is created with parameters: \n  VLAN %d \n", vid);
  209     l2_print_mac("  MAC Address: ", mac_addr);
  210     printf("\n\r");
  211     printf("  L3 Interface ID: %d\r\n", l3_intf.l3a_intf_id);
  212   }
  213 
  214   return rc;
  215 }
  216 /* __doxy_func_body_end__ */
  217 
  218 /**************************************************************************/
  237 int example_create_l3_egress(int unit, unsigned int flags, int out_port, int vlan,
  238     int l3_eg_intf, opennsl_mac_t next_hop_mac_addr,
  239     int *intf) {
  240   int rc;
  241   opennsl_l3_egress_t l3eg;
  242   opennsl_if_t l3egid;
  243   int mod = 0;
  244 
  245   opennsl_l3_egress_t_init(&l3eg);
  246   l3eg.intf = l3_eg_intf;
  247   memcpy(l3eg.mac_addr, next_hop_mac_addr, 6);
  248 
  249   l3eg.vlan   = vlan;
  250   l3eg.module = mod;
  251   l3eg.port   = out_port;
  252 
  253   l3egid = *intf;
  254 
  255   rc = opennsl_l3_egress_create(unit, flags, &l3eg, &l3egid);
  256   if (rc != OPENNSL_E_NONE) {
  257     return rc;
  258   }
  259 
  260   *intf = l3egid;
  261 
  262   if(verbose >= 2) {
  263     printf("Created L3 egress ID %d for out_port: %d vlan: %d "
  264         "L3 egress intf: %d\n",
  265         *intf, out_port, vlan, l3_eg_intf);
  266   }
  267 
  268   return rc;
  269 }
  270 /* __doxy_func_body_end__ */
  271 
  272 /**************************************************************************/
  281 int example_add_host(int unit, unsigned int addr, int intf) {
  282   int rc;
  283   opennsl_l3_host_t l3host;
  284   opennsl_l3_host_t_init(&l3host);
  285 
  286   l3host.l3a_flags = 0;
  287   l3host.l3a_ip_addr = addr;
  288   l3host.l3a_intf = intf;
  289   l3host.l3a_port_tgid = 0;
  290 
  291   rc = opennsl_l3_host_add(unit, &l3host);
  292   if (rc != OPENNSL_E_NONE) {
  293     printf ("opennsl_l3_host_add failed. Return Code: %s \n",
  294         opennsl_errmsg(rc));
  295     return rc;
  296   }
  297 
  298   if(verbose >= 1) {
  299     print_ip_addr("add host ", addr);
  300     printf(" ---> egress-object = %d\n", intf);
  301   }
  302 
  303   return rc;
  304 }
  305 /* __doxy_func_body_end__ */
  306 
  307 /**************************************************************************/
  320 int example_set_default_route(int unit, int subnet, int mask,
  321     int intf, opennsl_gport_t trap_port) {
  322   int rc;
  323   opennsl_l3_route_t l3rt;
  324 
  325   opennsl_l3_route_t_init(&l3rt);
  326 
  327   l3rt.l3a_flags = 0;
  328 
  329   /* to indicate it's default route, set subnet to zero */
  330   l3rt.l3a_subnet = subnet;
  331   l3rt.l3a_ip_mask = mask;
  332 
  333   l3rt.l3a_intf = intf;
  334   l3rt.l3a_port_tgid = trap_port;
  335 
  336   rc = opennsl_l3_route_add(unit, &l3rt);
  337   if (rc != OPENNSL_E_NONE) {
  338     printf ("opennsl_l3_route_add failed. Return Code: %s \n",
  339         opennsl_errmsg(rc));
  340     return rc;
  341   }
  342 
  343   if(verbose >= 1) {
  344     print_ip_addr("add default route for subnet ", subnet);
  345     print_ip_addr(" with mask ", mask);
  346     printf(" ---> egress-object = %d\n", intf);
  347   }
  348   return rc;
  349 }
  350 /* __doxy_func_body_end__ */
  351 
  352 /*****************************************************************/
  359 int main(int argc, char *argv[])
  360 {
  361   int rv = 0;
  362   int unit = 0;
  363   int choice;
  364   int ing_intf_in = 0;
  365   int ing_intf_out = 0;
  366   int l3egid;
  367   int flags = 0;
  368   int open_vlan = 1;
  369   int in_sysport;
  370   int out_sysport;
  371   int in_vlan  = INGRESS_VLAN;
  372   int out_vlan = EGRESS_VLAN;
  373   int host, subnet, mask;
  374   opennsl_mac_t my_mac  = MY_MAC;  /* my-MAC */
  375   opennsl_mac_t next_hop_mac  = NEXTHOP_MAC; /* next_hop_mac1 */
  376   unsigned int warm_boot;
  377   int index = 0;
  378 
  379   if(strcmp(argv[0], "gdb") == 0)
  380   {
  381     index = 1;
  382   }
  383   if((argc != (index + 3)) || ((argc > (index + 1)) 
  384         && (strcmp(argv[index + 1], "--help") == 0))) {
  385     printf("%s\n\r", example_usage);
  386     return OPENNSL_E_PARAM;
  387   }
  388 
  389   /* Initialize the system */
  390   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  391 
  392   if(rv != 0) {
  393     printf("\r\nFailed to initialize the system.\r\n");
  394     return rv;
  395   }
  396 
  397   /* Extract inputs parameters */
  398   in_sysport   = atoi(argv[index + 1]);
  399   out_sysport  = atoi(argv[index + 2]);
  400 
  401   warm_boot = opennsl_driver_boot_flags_get() & OPENNSL_BOOT_F_WARM_BOOT;
  402 
  403   if(!warm_boot)
  404   {
  405     /* cold boot initialization commands */
  406     rv = example_port_default_config(unit);
  407     if (rv != OPENNSL_E_NONE) {
  408       printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  409              rv, opennsl_errmsg(rv));
  410     }
  411 
  412     /* Set L3 Egress Mode */
  413     rv =  opennsl_switch_control_set(unit, opennslSwitchL3EgressMode, 1);
  414     if (rv != OPENNSL_E_NONE) {
  415       return rv;
  416     }
  417     if(verbose >= 3) {
  418       printf("\nL3 Egress mode is set succesfully\n");
  419     }
  420 
  421     /*** create ingress router interface ***/
  422     rv = example_create_l3_intf(unit, open_vlan, in_sysport, in_vlan,
  423         my_mac, &ing_intf_in);
  424     if (rv != OPENNSL_E_NONE) {
  425       printf("Error, create ingress interface-1, in_sysport=%d. "
  426           "Return code %s \n", in_sysport, opennsl_errmsg(rv));
  427       return rv;
  428     }
  429 
  430     /*** create egress router interface ***/
  431     rv = example_create_l3_intf(unit, open_vlan, out_sysport, out_vlan,
  432         my_mac, &ing_intf_out);
  433     if (rv != OPENNSL_E_NONE) {
  434       printf("Error, create egress interface-1, out_sysport=%d. "
  435           "Return code %s \n", out_sysport, opennsl_errmsg(rv));
  436       return rv;
  437     }
  438 
  439     /*** Make the address learn on a VLAN and port */
  440     rv = example_l2_addr_add(unit, my_mac, in_sysport, in_vlan);
  441     if (rv != OPENNSL_E_NONE) {
  442       printf("Failed to add L2 address. Return Code: %s\n", opennsl_errmsg(rv));
  443       return rv;
  444     }
  445 
  446     rv = example_l2_addr_add(unit, my_mac, out_sysport, out_vlan);
  447     if (rv != OPENNSL_E_NONE) {
  448       printf("Failed to add L2 address. Return Code: %s\n", opennsl_errmsg(rv));
  449       return rv;
  450     }
  451 
  452     /*** create egress object 1 ***/
  453     flags = 0;
  454     rv = example_create_l3_egress(unit, flags, out_sysport, out_vlan, ing_intf_out,
  455         next_hop_mac, &l3egid);
  456     if (rv != OPENNSL_E_NONE) {
  457       printf("Error, create egress object, out_sysport=%d. Return code %s \n",
  458           out_sysport, opennsl_errmsg(rv));
  459       return rv;
  460     }
  461 
  462     /*** add host point ***/
  463     host = HOST1;
  464     rv = example_add_host(unit, host, l3egid);
  465     if (rv != OPENNSL_E_NONE) {
  466       printf("Error, host add. Return code %s\n",
  467           opennsl_errmsg(rv));
  468       return rv;
  469     }
  470 
  471 
  472     /* With ALPM enabled, switch expects a default route to be 
  473      installed before adding a LPM route */
  474     subnet = 0x00000000;
  475     mask   = 0x00000000;
  476     rv = example_set_default_route(unit, subnet, mask, l3egid, 0);
  477     if (rv != OPENNSL_E_NONE) {
  478       printf("Error, default route add. Return code %s \n",
  479           opennsl_errmsg(rv));
  480       return rv;
  481     }
  482 
  483     /*** add default route ***/
  484     subnet = DEFAULT_SUBNET_IP;
  485     mask   = DEFAULT_SUBNET_MASK;
  486     rv = example_set_default_route(unit, subnet, mask, l3egid, 0);
  487     if (rv != OPENNSL_E_NONE) {
  488       printf("Error, default route add. Return code %s\n",
  489           opennsl_errmsg(rv));
  490       return rv;
  491     }
  492   }
  493 
  494   while(1) {
  495     printf("\r\nUser menu: Select one of the following options\r\n");
  496     printf("1. Save the configuration to scache\n");
  497 #ifdef INCLUDE_DIAG_SHELL
  498     printf("9. Launch diagnostic shell\n");
  499 #endif
  500     printf("0. Quit the application.\r\n");
  501 
  502     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  503     {
  504         printf("Invalid option entered. Please re-enter.\n");
  505         continue;
  506     }
  507     switch(choice)
  508     {
  509       case 1:
  510       {
  511         /* Sync the current configuration */
  512         rv = opennsl_switch_control_set(unit, opennslSwitchControlSync, 1);
  513         if(rv != OPENNSL_E_NONE) {
  514           printf("Failed to synchronize the configuration to scache. "
  515               "Error %s\n", opennsl_errmsg(rv));
  516           return rv;
  517         }
  518         printf("Warmboot configuration is saved successfully.\n");
  519         break;
  520       } /* End of case 1 */
  521 
  522 #ifdef INCLUDE_DIAG_SHELL
  523       case 9:
  524       {
  525         opennsl_driver_shell();
  526         break;
  527       }
  528 #endif
  529 
  530       case 0:
  531       {
  532         printf("Exiting the application.\n");
  533         rv = opennsl_driver_exit();
  534         return rv;
  535       }
  536       default:
  537       break;
  538     } /* End of switch */
  539   } /* End of while */
  540 
  541   return rv;
  542 }
  543 /* __doxy_func_body_end__ */
