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
   19  * \file         example_knet.c
   20  *
   21  * \brief        OpenNSL example application to demonstrate KNET module
   22  *
   23  * \details      This application allows the user to
   24  *               1) Create KNET interfaces that map to switch physical
   25  *               interfaces. These can be configurable from Linux command
   26  *               line interface.
   27  *               2) Delete KNET interface.
   28  *               3) List KNET interfaces.
   29  *
   30  **********************************************************************/
   31 #include <stdio.h>
   32 #include <stdlib.h>
   33 #include <string.h>
   34 #include <sal/driver.h>
   35 #include <opennsl/error.h>
   36 #include <opennsl/vlan.h>
   37 #include <opennsl/rx.h>
   38 #include <opennsl/knet.h>
   39 #include <examples/util.h>
   40 
   41 #define DEFAULT_UNIT 0
   42 #define DEFAULT_PORT 1
   43 #define DEFAULT_VLAN 1
   44 #define MAX_DIGITS_IN_CHOICE 5
   45 #define KNET_INTF_COUNT 20
   46 #define IP_ADDR_LEN 16
   47 #define NETMASK_LEN 16
   48 
   49 char example_usage[] =
   50 "Syntax: example_knet                                                  \n\r"
   51 "                                                                      \n\r"
   52 "Paramaters: None.                                                     \n\r"
   53 "                                                                      \n\r"
   54 "Example: The following command is used to create KNET interfaces      \n\r"
   55 "         that can be configurable from Linux shell.                   \n\r"
   56 "         example_knet                                                 \n\r"
   57 "                                                                      \n\r"
   58 "Usage Guidelines: This program request the user to enter the following\n\r"
   59 "                  parameters interactively                            \n\r"
   60 "       port    - port number                                          \n\r"
   61 "       ifName  - interface name                                       \n\r"
   62 "       ipaddr  - IP adddress to be assigned to the interface          \n\r"
   63 "       netmask - IP netmask to be assigned to the interface           \n\r"
   64 "                                                                      \n\r"
   65 "       Testing:                                                       \n\r"
   66 "       1) Assign IP address to the interface using Create KNET        \n\r"
   67 "          interface option.                                           \n\r"
   68 "       2) Assign IP address to the partner that is connected to       \n\r"
   69 "          physical port in the same subnet.                           \n\r"
   70 "       3) Verify that ping to the IP assigned in step 1 works fine.   \n\r"
   71 "                                                                      \n\r";
   72 
   73 /* KNET data structure to store the netif.id and filter.id for
   74  * KNET interfaces */
   75 typedef struct
   76 {
   77   int port;
   78   int netIfID;
   79   int filterID;
   80   char name[OPENNSL_KNET_NETIF_NAME_MAX];
   81 } knet_if_data_t;
   82 /* __doxy_func_body_end__ */
   83 
   84 knet_if_data_t knet_if_data[KNET_INTF_COUNT];
   85 int knet_intf_count = 0;
   86 
   87 /**************************************************************************/
   97 void example_knet_database_update(int port, char *ifName, int nID, int fID)
   98 {
   99   int i;
  100   int entryFound = 0;
  101 
  102   for(i = 0; i < KNET_INTF_COUNT; i++)
  103   {
  104     /* Check if the entry for the given port is already present */
  105     if(port == knet_if_data[i].port)
  106     {
  107       entryFound = 1;
  108       break;
  109     }
  110   }
  111 
  112   if(entryFound == 0)
  113   {
  114     /* Find the first free entry */
  115     for(i = 0; i < KNET_INTF_COUNT; i++)
  116     {
  117       if(knet_if_data[i].port == 0)
  118       {
  119         break;
  120       }
  121     }
  122   }
  123 
  124   knet_if_data[i].port     = port;
  125   knet_if_data[i].netIfID  = nID;
  126   knet_if_data[i].filterID = fID;
  127   strcpy(knet_if_data[i].name, ifName);
  128 
  129   /* Updated the new entry. So, increment KNET interface count */
  130   if(entryFound == 0)
  131   {
  132     knet_intf_count++;
  133   }
  134   return;
  135 }
  136 /* __doxy_func_body_end__ */
  137 
  138 /**************************************************************************/
  143 void example_knet_database_show()
  144 {
  145   int i;
  146 
  147   if(knet_intf_count == 0)
  148   {
  149     printf("KNET interface table is empty.\n\r");
  150     return;
  151   }
  152 
  153   printf("Number of KNET interfaces present: %d\n\n\r", knet_intf_count);
  154   for(i = 0; i < KNET_INTF_COUNT; i++)
  155   {
  156     if(knet_if_data[i].port != 0)
  157     {
  158       printf("Port: %d Interface Name: %s netifID: %d filterID: %d\n\r",
  159           knet_if_data[i].port, knet_if_data[i].name,
  160           knet_if_data[i].netIfID, knet_if_data[i].filterID);
  161     }
  162   }
  163   return;
  164 }
  165 /* __doxy_func_body_end__ */
  166 
  167 /******************************************************************************
  168  * \brief     Creates linux interfaces and corresponding KNET filters to map
  169  *           each front panel interface to these linux interfaces.
  170  *
  171  * \param   port         [IN] port number
  172  * \param   unit         [IN] unit number
  173  * \param   ifName       [IN] interface name
  174  *
  175  * \return  None
  176  *
  177  *****************************************************************************/
  178 int example_linux_interface_create(int unit, int port, char *ifName)
  179 {
  180   int rv;
  181   opennsl_knet_netif_t  netif;
  182   opennsl_knet_filter_t filter;
  183   unsigned char baseMac[6] = { 0x02, 0x10, 0x18, 0x00, 0x00, 0x01 };
  184 
  185   opennsl_knet_netif_t_init(&netif);
  186   netif.type = OPENNSL_KNET_NETIF_T_TX_LOCAL_PORT;
  187   netif.port = port; /* Egress port */
  188   strcpy(netif.name, ifName); /* Copy the base interface name */
  189   /* Set MAC address associated with this interface */
  190   memcpy(netif.mac_addr, baseMac, 6);
  191   rv = opennsl_knet_netif_create(unit, &netif);
  192   if (rv >= 0)
  193   {
  194     printf("Created Interface ID %d for interface name \"%s\" \n",
  195         netif.id, ifName);
  196   }
  197   else
  198   {
  199     printf("rv = %d Error creating KNET interface \"%s\" \n", rv, ifName);
  200     return rv;
  201   }
  202 
  203   /* Add filter for packets from port */
  204   opennsl_knet_filter_t_init(&filter);
  205 
  206   filter.type = OPENNSL_KNET_FILTER_T_RX_PKT;
  207 
  208   /*
  209      This filter is modified to **NOT** STRIP VLAN TAG when this port is
  210      attached to a bridge
  211    */
  212 
  213   filter.flags = OPENNSL_KNET_FILTER_F_STRIP_TAG;
  214   filter.priority = 0;
  215   /* Send packet to network interface */
  216   filter.dest_type = OPENNSL_KNET_DEST_T_NETIF;
  217   filter.dest_id = netif.id;
  218   filter.match_flags = OPENNSL_KNET_FILTER_M_INGPORT;
  219   filter.m_ingport = port;
  220 
  221   rv = opennsl_knet_filter_create(unit, &filter);
  222   if (rv >= 0)
  223   {
  224     printf("Created Filter ID %d for interface name \"%s\" \n",
  225         filter.id, ifName);
  226   }
  227   else
  228   {
  229     printf("rv  = %d Error creating filter for interface \"%s\" \n",
  230         rv, ifName);
  231     return rv;
  232   }
  233 
  234   /* Update the database */
  235   example_knet_database_update(port, ifName, netif.id, filter.id);
  236 
  237   return rv;
  238 }
  239 /* __doxy_func_body_end__ */
  240 
  241 /******************************************************************************
  242  * \brief   Configure IP address and netmask on the KNET interface.
  243  *
  244  * \param   port         [IN] port number
  245  * \param   unit         [IN] unit number
  246  * \param   ifName       [IN] interface name
  247  *
  248  * \return  None
  249  *
  250  *****************************************************************************/
  251 void example_ip_config(char *ifName, char* ip, char* netmask)
  252 {
  253   char cmd[100];
  254 
  255   memset(cmd, 0, sizeof(cmd));
  256 
  257   sprintf(cmd, "ifconfig %s %s netmask %s", ifName, ip, netmask);
  258   if(system(cmd) == -1)
  259   {
  260     printf("Failed to configure interface \"%s\" with IP: %s Netmask: %s\n",
  261         ifName, ip, netmask);
  262     return;
  263   };
  264 
  265   sprintf(cmd, "ifconfig %s up > /dev/null", ifName);
  266   if(system(cmd) == -1)
  267   {
  268     printf("Failed to bringup interface \"%s\"\n", ifName);
  269     return;
  270   };
  271 
  272   printf("Configured interface \"%s\" with IP: %s Netmask: %s\n",
  273       ifName, ip, netmask);
  274 }
  275 /* __doxy_func_body_end__ */
  276 
  277 /******************************************************************************
  278  * \brief   To create KNET interface by taking inputs from the user
  279  *
  280  * \param   unit         [IN] unit number
  281  *
  282  * \return  None
  283  *
  284  *****************************************************************************/
  285 void example_knet_intf_create(int unit)
  286 {
  287   int port;
  288   int rv;
  289   char ifName[OPENNSL_KNET_NETIF_NAME_MAX];
  290   char ip[IP_ADDR_LEN];
  291   char netmask[NETMASK_LEN];
  292 
  293   if(knet_intf_count == KNET_INTF_COUNT)
  294   {
  295     printf("Maximum number of KNET interfaces are already present.\r\n");
  296     return;
  297   }
  298 
  299   printf("\n\rEnter port number: \n");
  300   if(example_read_user_choice(&port) != OPENNSL_E_NONE)
  301   {
  302     printf("Invalid option entered.\n");
  303     return;
  304   }
  305   printf("\n\rEnter knet interface name: ");
  306   if(example_read_user_string(ifName, sizeof(ifName)) == NULL)
  307   {
  308     printf("\n\rInvalid KNET interface is entered.\n\r");
  309     return;
  310   }
  311   printf("\n\rEnter IP address in dotted decimal format: ");
  312   if(example_read_user_string(ip, sizeof(ip)) == NULL)
  313   {
  314     printf("\n\rInvalid IP address is entered.\n\r");
  315     return;
  316   }
  317   printf("\n\rEnter IP netmask in dotted decimal format: ");
  318   if(example_read_user_string(netmask, sizeof(netmask)) == NULL)
  319   {
  320     printf("\n\rInvalid IP netmask is entered.\n\r");
  321     return;
  322   }
  323 
  324   /* Create Linux interface. */
  325   rv = example_linux_interface_create(unit, port, ifName);
  326   if(rv < 0) {
  327     printf("Failed to create Linux interface \"%s\". rv: %d\r\n", ifName, rv);
  328     return;
  329   }
  330 
  331   printf("Created Linux interface \"%s\" successfully.\r\n", ifName);
  332   example_ip_config(ifName, ip, netmask);
  333   return;
  334 }
  335 /* __doxy_func_body_end__ */
  336 
  337 /******************************************************************************
  338  * \brief   To delete KNET interface.
  339  *
  340  * \param   unit         [IN] unit number
  341  *
  342  * \return  None
  343  *
  344  *****************************************************************************/
  345 void example_knet_intf_delete(int unit)
  346 {
  347   int rv;
  348   int i;
  349   char ifName[OPENNSL_KNET_NETIF_NAME_MAX];
  350 
  351   if(knet_intf_count == 0)
  352   {
  353     printf("\n\rThere are no KNET interfaces present in the system.\n\r");
  354   }
  355   printf("\n\rEnter KNET interface name: ");
  356   if(example_read_user_string(ifName, sizeof(ifName)) == NULL)
  357   {
  358     printf("\n\rInvalid KNET interface is entered.\n\r");
  359     return;
  360   }
  361 
  362   for(i = 0; i < KNET_INTF_COUNT; i++)
  363   {
  364     if(strcmp(ifName, knet_if_data[i].name) == 0)
  365     {
  366       break;
  367     }
  368   }
  369 
  370   if (i == KNET_INTF_COUNT)
  371   {
  372     printf("\n\rThe specific interface is not found.\n\r");
  373     return;
  374   }
  375 
  376   /* Destroy KNET interface. */
  377   rv = opennsl_knet_netif_destroy(unit, knet_if_data[i].netIfID);
  378   if(rv < 0) {
  379     printf("Error destroying network interface: %s\n",
  380         opennsl_errmsg(rv));
  381     return;
  382   }
  383 
  384   rv = opennsl_knet_filter_destroy(unit, knet_if_data[i].filterID);
  385   if(rv < 0) {
  386     printf("Error destroying packet filter: %s\n",
  387         opennsl_errmsg(rv));
  388     return;
  389   }
  390 
  391   /* Clear the database for the deleted port */
  392   memset(&knet_if_data[i], 0, sizeof(knet_if_data_t));
  393   knet_intf_count--;
  394 }
  395 /* __doxy_func_body_end__ */
  396 
  397 /*****************************************************************/
  404 int main(int argc, char *argv[])
  405 {
  406   int rv = 0;
  407   int unit = DEFAULT_UNIT;
  408   int choice;
  409   opennsl_port_config_t pcfg;
  410 
  411   if((argc != 1) || ((argc > 1) && (strcmp(argv[1], "--help") == 0))) {
  412     printf("%s\n\r", example_usage);
  413     return OPENNSL_E_PARAM;
  414   }
  415 
  416   /* Initialize the system */
  417   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  418 
  419   if(rv != OPENNSL_E_NONE) {
  420     printf("\r\nFailed to initialize the system. Error %s\r\n",
  421         opennsl_errmsg(rv));
  422     return rv;
  423   }
  424 
  425   /* rx start */
  426   if (!opennsl_rx_active(unit))
  427   {
  428     rv = opennsl_rx_start(unit, NULL);
  429     if (OPENNSL_FAILURE(rv))
  430     {
  431       printf("RX start failed. rv = %s\n", opennsl_errmsg(rv));
  432       return rv;
  433     }
  434   }
  435 
  436   /* cold boot initialization commands */
  437   rv = example_port_default_config(unit);
  438   if (rv != OPENNSL_E_NONE) {
  439     printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  440            rv, opennsl_errmsg(rv));
  441   }
  442 
  443   /* Add ports to default vlan. */
  444   printf("Adding ports to default VLAN %d.\r\n", DEFAULT_VLAN);
  445   rv = example_switch_default_vlan_config(unit);
  446   if(rv != OPENNSL_E_NONE) {
  447     printf("\r\nFailed to add ports to default VLAN %d. rv: %d\r\n",
  448         DEFAULT_VLAN,  rv);
  449     return rv;
  450   }
  451 
  452   /* Add CPU to default vlan. */
  453   rv = opennsl_port_config_get(unit, &pcfg);
  454   if (rv != OPENNSL_E_NONE) {
  455     printf("Failed to get port configuration. Error %s\n", opennsl_errmsg(rv));
  456     return rv;
  457   }
  458 
  459   rv = opennsl_vlan_port_add(unit, DEFAULT_VLAN, pcfg.cpu, pcfg.cpu);
  460   if (rv != OPENNSL_E_NONE) {
  461     printf("Failed to add ports to VLAN. Error %s\n", opennsl_errmsg(rv));
  462     return rv;
  463   }
  464 
  465   while(1) {
  466 
  467     printf("\nUser Menu: Select one of the following options\n");
  468     printf("1. Create KNET interface\n");
  469     printf("2. Delete KNET interface\n");
  470     printf("3. Display KNET interface database\n");
  471 #ifdef INCLUDE_DIAG_SHELL
  472     printf("9. Launch diagnostic shell\n");
  473 #endif
  474     printf("0. Quit the application.\n");
  475 
  476     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  477     {
  478       printf("Invalid option entered. Please re-enter.\n");
  479       continue;
  480     }
  481     switch(choice) {
  482       case 1:
  483       {
  484         example_knet_intf_create(unit);
  485         break;
  486       }
  487 
  488       case 2:
  489       {
  490         example_knet_intf_delete(unit);
  491         break;
  492       }
  493 
  494       case 3:
  495       {
  496         example_knet_database_show();
  497         break;
  498       }
  499 
  500 #ifdef INCLUDE_DIAG_SHELL
  501       case 9:
  502       {
  503         opennsl_driver_shell();
  504         break;
  505       }
  506 #endif
  507 
  508       case 0:
  509       {
  510         printf("Exiting the application.\n");
  511         rv = opennsl_driver_exit();
  512         return rv;
  513       }
  514 
  515       default:
  516         break;;
  517     } /* End of switch */
  518   } /* End of while */
  519 
  520   return rv;
  521 }
  522 /* __doxy_func_body_end__ */
