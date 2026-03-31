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
   18  * \file     example_trunk.c
   19  *
   20  * \brief    Example code for Trunk application
   21  *
   22  * \details  This example demonstrates a simple trunk API scenario.
   23  *
   24  * This application also supports Warmboot feature. Warmboot is the
   25  * process of restarting the device driver software while the hardware is
   26  * running without interrupting the dataplane.
   27  *
   28  * Setup the following envinonment variable before running the application.
   29  * For Cold boot mode, use "export OPENNSL_BOOT_FLAGS = 0x000000".
   30  * For Warm boot mode, use "export OPENNSL_BOOT_FLAGS = 0x200000".
   31  *
   32  ************************************************************************/
   33 
   34 #include <stdio.h>
   35 #include <stdlib.h>
   36 #include <string.h>
   37 #include <sal/driver.h>
   38 #include <opennsl/error.h>
   39 #include <opennsl/vlan.h>
   40 #include <opennsl/port.h>
   41 #include <opennsl/switch.h>
   42 #include <opennsl/trunk.h>
   43 #include <examples/util.h>
   44 
   45 #define DEFAULT_UNIT  0
   46 #define DEFAULT_VLAN  1
   47 #define MAX_DIGITS_IN_CHOICE 5
   48 #define MAX_TRUNK_MEMBER 8
   49 
   50 char example_usage[] =
   51 "Syntax: example_trunk                                                 \n\r"
   52 "                                                                      \n\r"
   53 "Paramaters: None.                                                     \n\r"
   54 "                                                                      \n\r"
   55 "Example: The following command is used to demonstrate trunk API       \n\r"
   56 "calls. The interactive user interface allows the user to get          \n\r"
   57 "the trunk information, add, delete ports and destroy a trunk.         \n\r"
   58 "         example_trunk                                                \n\r"
   59 "                                                                      \n\r"
   60 "Usage Guidelines: None.                                               \n\r";
   61 
   62 /*****************************************************************/
   72 int example_trunk_create(int unit, opennsl_trunk_t *tid)
   73 {
   74   opennsl_error_t rv;
   75 
   76   rv = opennsl_trunk_create(unit, 0, tid);
   77   return rv;
   78 }
   79 /* __doxy_func_body_end__ */
   80 
   81 /*****************************************************************/
   88 int example_trunk_hash_controls_set(int unit)
   89 {
   90   opennsl_error_t rv = OPENNSL_E_NONE;
   91   int hashControl = 0;
   92 
   93   /* Set the ECMP hashControl to include dstip also - for XGS3 only */
   94   OPENNSL_IF_ERROR_RETURN(opennsl_switch_control_get(unit,
   95         opennslSwitchHashControl, &hashControl));
   96 
   97   /* Set the L2 hash controls first. */
   98   hashControl |= (OPENNSL_HASH_CONTROL_TRUNK_NUC_DST
   99       | OPENNSL_HASH_CONTROL_TRUNK_NUC_SRC
  100       | OPENNSL_HASH_CONTROL_TRUNK_UC_SRCPORT);
  101 
  102   OPENNSL_IF_ERROR_RETURN(opennsl_switch_control_set(unit,
  103         opennslSwitchHashControl, hashControl));
  104 
  105   /* Set the L3 hash controls next. */
  106   /* L4 ports hashControl below is valid for ECMP as well as regular UC
  107    * trunk load balacing
  108    */
  109   hashControl |=  (OPENNSL_HASH_CONTROL_MULTIPATH_L4PORTS
  110       | OPENNSL_HASH_CONTROL_MULTIPATH_DIP);
  111 
  112   OPENNSL_IF_ERROR_RETURN(opennsl_switch_control_set(unit,
  113         opennslSwitchHashControl, hashControl));
  114 
  115   return rv;
  116 }
  117 /* __doxy_func_body_end__ */
  118 
  119 /*****************************************************************/
  126 int main(int argc, char *argv[])
  127 {
  128   opennsl_error_t   rv;
  129   opennsl_trunk_t tid;
  130   opennsl_gport_t gport;
  131   opennsl_port_t port;
  132   int unit = DEFAULT_UNIT;
  133   int member_max = MAX_TRUNK_MEMBER, member_count, i;
  134   opennsl_trunk_member_t member;
  135   opennsl_trunk_member_t member_arr[member_max];
  136   opennsl_trunk_info_t t_data;
  137   unsigned int warm_boot;
  138   int choice;
  139   int index = 0;
  140   int psc = 0;
  141 
  142   if(strcmp(argv[0], "gdb") == 0)
  143   {
  144     index = 1;
  145   }
  146 
  147   if((argc != (index + 1)) || ((argc > (index + 1)) && (strcmp(argv[index + 1], "--help") == 0))) {
  148     printf("%s\n\r", example_usage);
  149     return OPENNSL_E_PARAM;
  150   }
  151 
  152   /* Initialize the system. */
  153   printf("Initializing the system.\r\n");
  154   rv = opennsl_driver_init((opennsl_init_t *) NULL);
  155 
  156   if(rv != OPENNSL_E_NONE) {
  157     printf("\r\nFailed to initialize the system. Error %s\r\n",
  158         opennsl_errmsg(rv));
  159     return rv;
  160   }
  161 
  162   warm_boot = opennsl_driver_boot_flags_get() & OPENNSL_BOOT_F_WARM_BOOT;
  163 
  164   if(!warm_boot)
  165   {
  166    /* cold boot initialization commands */
  167     rv = example_port_default_config(unit);
  168     if (rv != OPENNSL_E_NONE)
  169     {
  170       printf("\r\nFailed to apply default config on ports, rc = %d (%s).\r\n",
  171              rv, opennsl_errmsg(rv));
  172     }
  173 
  174     /* Add ports to default vlan. */
  175     printf("Adding ports to default vlan.\r\n");
  176     rv = example_switch_default_vlan_config(unit);
  177     if(rv != OPENNSL_E_NONE)
  178     {
  179       printf("\r\nFailed to add default ports. rv: %s\r\n", opennsl_errmsg(rv));
  180       return rv;
  181     }
  182 
  183     printf("Setting Hash controls for Trunk.\r\n");
  184     rv = example_trunk_hash_controls_set(unit);
  185     if(rv != OPENNSL_E_NONE)
  186     {
  187       printf("\r\nFailed to hash control for Trunk. Error %s\r\n",
  188           opennsl_errmsg(rv));
  189     }
  190   }
  191 
  192   while (1) {
  193     printf("\r\nUser menu: Select one of the following options\r\n");
  194     printf("1. Create trunk\n");
  195     printf("2. Add port to trunk\n");
  196     printf("3. Delete port from trunk\n");
  197     printf("4. Get trunk info\n");
  198     printf("5. Delete trunk\n");
  199     printf("6. Save the configuration to scache\n");
  200     printf("7. Set port selection criteria (PSC)\n");
  201 #ifdef INCLUDE_DIAG_SHELL
  202     printf("9. Launch diagnostic shell\n");
  203 #endif
  204     printf("0. Quit the application\n");
  205 
  206     if(example_read_user_choice(&choice) != OPENNSL_E_NONE)
  207     {
  208         printf("Invalid option entered. Please re-enter.\n");
  209         continue;
  210     }
  211     switch(choice){
  212 
  213       case 1:
  214       {
  215         /* Create default trunk */
  216         rv = example_trunk_create(unit, &tid);
  217         if (rv == OPENNSL_E_NONE) {
  218           printf("Trunk %d created successfully\n", tid);
  219         } else {
  220           printf("Failed to create trunk. Error %s\n",
  221                  opennsl_errmsg(rv));
  222           continue;
  223         }
  224 
  225         break;
  226       } /* End of case 1 */
  227 
  228       case 2:
  229       {
  230         /* Add port to trunk*/
  231         printf("\r\nEnter the port number.\r\n");
  232         if(example_read_user_choice(&port) != OPENNSL_E_NONE)
  233         {
  234             printf("Invalid option entered. Please re-enter.\n");
  235             continue;
  236         }
  237         rv = opennsl_port_gport_get (unit, port, &gport);
  238         if (rv != OPENNSL_E_NONE)
  239         {
  240           return OPENNSL_E_FAIL;
  241         }
  242 
  243         opennsl_trunk_member_t_init(&member);
  244         member.gport = gport;
  245 
  246         printf("\r\nEnter the trunk id.\r\n");
  247         if(example_read_user_choice(&tid) != OPENNSL_E_NONE)
  248         {
  249             printf("Invalid option entered. Please re-enter.\n");
  250             continue;
  251         }
  252 
  253         printf("member_add: Adding port %d to trunk-id %d\n", port, tid);
  254         rv = opennsl_trunk_member_add(unit, tid, &member);
  255         if ((rv == OPENNSL_E_NONE)) {
  256           printf("Port %d added successfully to trunk %d\n", port, tid);
  257         } else {
  258           printf("Failed to add port. Error %s\n",
  259                  opennsl_errmsg(rv));
  260           continue;
  261         }
  262 
  263         break;
  264       } /* End of case 2 */
  265 
  266       case 3:
  267       {
  268         /* Delete port from trunk*/
  269         printf("\r\nEnter the port number.\r\n");
  270         if(example_read_user_choice(&port) != OPENNSL_E_NONE)
  271         {
  272             printf("Invalid option entered. Please re-enter.\n");
  273             continue;
  274         }
  275         rv = opennsl_port_gport_get (unit, port, &gport);
  276         if (rv != OPENNSL_E_NONE)
  277         {
  278           return OPENNSL_E_FAIL;
  279         }
  280 
  281         opennsl_trunk_member_t_init(&member);
  282         member.gport = gport;
  283 
  284         printf("\r\nEnter the trunk id.\r\n");
  285         if(example_read_user_choice(&tid) != OPENNSL_E_NONE)
  286         {
  287             printf("Invalid option entered. Please re-enter.\n");
  288             continue;
  289         }
  290 
  291         printf("member_delete: Deleting port %d from trunk-id %d\n", port, tid);
  292         rv = opennsl_trunk_member_delete(unit, tid, &member);
  293         if ((rv == OPENNSL_E_NONE)) {
  294           printf("Port %d deleted successfully from trunk %d\n", port, tid);
  295         } else {
  296           printf("Failed to delete port. Error %s\n",
  297                  opennsl_errmsg(rv));
  298           continue;
  299         }
  300 
  301         break;
  302       } /* End of case 3 */
  303 
  304       case 4:
  305       {
  306         /* Get trunk info*/
  307         printf("\r\nEnter the trunk id.\r\n");
  308         if(example_read_user_choice(&tid) != OPENNSL_E_NONE)
  309         {
  310             printf("Invalid option entered. Please re-enter.\n");
  311             continue;
  312         }
  313 
  314         for (i = 0; i < member_max; i++)
  315         {
  316           opennsl_trunk_member_t_init(&member_arr[i]);
  317         }
  318         opennsl_trunk_info_t_init(&t_data);
  319 
  320         rv = opennsl_trunk_get(unit, tid, &t_data, member_max,
  321             member_arr, &member_count);
  322         if ((rv != OPENNSL_E_NONE)) {
  323           printf("Failed to get trunk info. Error %s\n",
  324                  opennsl_errmsg(rv));
  325           continue;
  326         }
  327         printf("trunk %d has %d members, psc %d\n", tid ,member_count, t_data.psc);
  328         for( i = 0; i < member_count; i++)
  329         {
  330           opennsl_port_local_get(unit, member_arr[i].gport, &port);
  331           printf("port %d\n",port);
  332         }
  333 
  334         break;
  335       } /* End of case 4 */
  336 
  337       case 5:
  338       {
  339         /* Delete a trunk */
  340         printf("\r\nEnter the trunk id.\r\n");
  341         if(example_read_user_choice(&tid) != OPENNSL_E_NONE)
  342         {
  343             printf("Invalid option entered. Please re-enter.\n");
  344             continue;
  345         }
  346 
  347         rv = opennsl_trunk_destroy(unit, tid);
  348         if ((rv == OPENNSL_E_NONE)) {
  349           printf("trunk %d successfully deleted\n", tid);
  350         } else {
  351           printf("Failed to delete trunk. Error %s\n",
  352                  opennsl_errmsg(rv));
  353           continue;
  354         }
  355 
  356         break;
  357       } /* End of case 5 */
  358 
  359       case 6:
  360       {
  361         /* Sync the current configuration */
  362         rv = opennsl_switch_control_set(DEFAULT_UNIT, opennslSwitchControlSync, 1);
  363         if(rv != OPENNSL_E_NONE) {
  364           printf("Failed to synchronize the configuration to scache. "
  365               "Error %s\n", opennsl_errmsg(rv));
  366           return rv;
  367         }
  368         printf("Warmboot configuration is saved successfully.\n");
  369         break;
  370       } /* End of case 6 */
  371 
  372       case 7:
  373       {
  374         printf("\r\nEnter the trunk id.\r\n");
  375         if(example_read_user_choice(&tid) != OPENNSL_E_NONE)
  376         {
  377             printf("Invalid option entered. Please re-enter.\n");
  378             continue;
  379         }
  380 
  381         printf("\r\nEnter the PSC.\r\n");
  382         if(example_read_user_choice(&psc) != OPENNSL_E_NONE)
  383         {
  384             printf("Invalid option entered. Please re-enter.\n");
  385             continue;
  386         }
  387 
  388         rv = opennsl_trunk_psc_set(unit, tid, psc);
  389         if(rv != OPENNSL_E_NONE) {
  390           printf("Failed to set the PSC. Error %s\n", opennsl_errmsg(rv));
  391         }
  392 
  393         break;
  394       }
  395 
  396 #ifdef INCLUDE_DIAG_SHELL
  397       case 9:
  398       {
  399         opennsl_driver_shell();
  400         break;
  401       }
  402 #endif
  403 
  404       case 0:
  405       {
  406         printf("Exiting the application.\n");
  407         rv = opennsl_driver_exit();
  408         return rv;
  409       }
  410 
  411       default:
  412         break;
  413     } /* End of switch */
  414   } /* End of while */
  415 
  416   return OPENNSL_E_NONE;
  417 }
  418 /* __doxy_func_body_end__ */
