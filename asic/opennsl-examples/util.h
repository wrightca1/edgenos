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
   18  * \file     util.h
   19  *
   20  * \brief    OpenNSL utility routines required for example applications
   21  *
   22  * \details  OpenNSL utility routines required for example applications
   23  *
   24  ************************************************************************/
   25 #ifndef _OPENNSL_EXAMPLE_UTIL_H
   26 #define _OPENNSL_EXAMPLE_UTIL_H
   27 
   28 #include <stdio.h>
   29 #include <stdlib.h>
   30 #include <string.h>
   31 #include <sal/driver.h>
   32 
   33 #define OPENNSL_BOOT_F_DEFAULT          0x000000
   34 #define OPENNSL_BOOT_F_WARM_BOOT        0x200000
   35 
   36 /*************************************************************************/
   46 int example_is_dnx_device(int unit);
   47 
   48 /*************************************************************************/
   58 int example_is_qmx_device(int unit);
   59 
   60 /*****************************************************************/
   68 int example_port_default_config(int unit);
   69 
   70 /*****************************************************************/
   77 int example_switch_default_vlan_config(int unit);
   78 
   79 /**************************************************************************/
   87  char *example_read_user_string(char *buf, size_t buflen);
   88 
   89 /**************************************************************************/
   96 int example_read_user_choice(int *choice);
   97 
   98 /*****************************************************************/
  106 int opennsl_mac_parse(char *buf, unsigned char *macp);
  107 
  108 /*****************************************************************/
  113 void l2_print_mac(char *str, opennsl_mac_t mac_address);
  114 
  115 /*****************************************************************/
  123 int opennsl_ip_parse(char *ip_str, unsigned int *ip_val);
  124 
  125 /**************************************************************************/
  134 void print_ip_addr(char *str, unsigned int host);
  135 
  136 /**************************************************************************/
  144 int example_max_port_count_get(int unit, int *count);
  145 
  146 #endif /* _OPENNSL_EXAMPLE_UTIL_H */
