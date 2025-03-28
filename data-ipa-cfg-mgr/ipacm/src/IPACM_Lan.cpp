/*
Copyright (c) 2013-2016, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following
disclaimer in the documentation and/or other materials provided
with the distribution.

* Neither the name of The Linux Foundation nor the names of its
contributors may be used to endorse or promote products derived
from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/*!
	@file
	IPACM_Lan.cpp

	@brief
	This file implements the LAN iface functionality.

	@Author
	Skylar Chang

*/
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "IPACM_Netlink.h"
#include "IPACM_Lan.h"
#include "IPACM_Wan.h"
#include "IPACM_IfaceManager.h"
#include "linux/rmnet_ipa_fd_ioctl.h"
#include "linux/ipa_qmi_service_v01.h"
#include "linux/msm_ipa.h"
#include "IPACM_ConntrackListener.h"
#include <sys/ioctl.h>
#include <fcntl.h>

bool IPACM_Lan::odu_up = false;

IPACM_Lan::IPACM_Lan(int iface_index) : IPACM_Iface(iface_index)
{
	num_eth_client = 0;
	header_name_count = 0;
	ipv6_set = 0;
	ipv4_header_set = false;
	ipv6_header_set = false;
	odu_route_rule_v4_hdl = NULL;
	odu_route_rule_v6_hdl = NULL;
	eth_client = NULL;
	int i, m_fd_odu, ret = IPACM_SUCCESS;
	eth_client_len = 0;
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	int max_clients = (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable) ? IPA_MAX_NUM_HW_PATH_CLIENTS:
		IPA_MAX_NUM_ETH_CLIENTS;
#else
	int max_clients = IPA_MAX_NUM_ETH_CLIENTS;
#endif

	Nat_App = NatApp::GetInstance();
	if (Nat_App == NULL)
	{
		IPACMERR("unable to get Nat App instance \n");
		return;
	}

	num_wan_ul_fl_rule_v4 = 0;
	num_wan_ul_fl_rule_v6 = 0;
	hdr_len = 0;
	memset(wan_ul_fl_rule_hdl_v4, 0, MAX_WAN_UL_FILTER_RULES * sizeof(uint32_t));
	memset(wan_ul_fl_rule_hdl_v6, 0, MAX_WAN_UL_FILTER_RULES * sizeof(uint32_t));

	is_active = true;
	memset(ipv4_icmp_flt_rule_hdl, 0, NUM_IPV4_ICMP_FLT_RULE * sizeof(uint32_t));

	is_mode_switch = false;
	if_ipv4_subnet =0;
	memset(private_fl_rule_hdl, 0, IPA_MAX_PRIVATE_SUBNET_ENTRIES * sizeof(uint32_t));
	memset(ipv6_prefix_flt_rule_hdl, 0, NUM_IPV6_PREFIX_FLT_RULE * sizeof(uint32_t));
	memset(ipv6_icmp_flt_rule_hdl, 0, NUM_IPV6_ICMP_FLT_RULE * sizeof(uint32_t));
	modem_ul_v4_set = false;
	modem_ul_v6_set = false;
	memset(ipv6_prefix, 0, sizeof(ipv6_prefix));

	/* support eth multiple clients */
	if(iface_query != NULL)
	{
		if(ipa_if_cate != WLAN_IF)
		{
			eth_client_len = (sizeof(ipa_eth_client)) + (iface_query->num_tx_props * sizeof(eth_client_rt_hdl));
			eth_client = (ipa_eth_client *)calloc(max_clients, eth_client_len);
			if (eth_client == NULL)
			{
				IPACMERR("unable to allocate memory\n");
				return;
			}
		}

		IPACMDBG_H(" IPACM->IPACM_Lan(%d) constructor: Tx:%d Rx:%d \n", ipa_if_num,
					 iface_query->num_tx_props, iface_query->num_rx_props);

		/* ODU routing table initilization */
		if(ipa_if_cate == ODU_IF)
		{
			odu_route_rule_v4_hdl = (uint32_t *)calloc(iface_query->num_tx_props, sizeof(uint32_t));
			odu_route_rule_v6_hdl = (uint32_t *)calloc(iface_query->num_tx_props, sizeof(uint32_t));
			if ((odu_route_rule_v4_hdl == NULL) || (odu_route_rule_v6_hdl == NULL))
			{
				IPACMERR("unable to allocate memory\n");
				if(odu_route_rule_v4_hdl != NULL)
				{
					free(odu_route_rule_v4_hdl);
				}
				else if(odu_route_rule_v6_hdl != NULL)
				{
					free(odu_route_rule_v6_hdl);
				}
				return;
			}
		}
	}
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	for (i = 0; i < IPA_MAX_NUM_HW_PATH_CLIENTS; i++)
	{
		active_lan_client_index[i].lan_stats_idx = -1;
		memset(active_lan_client_index[i].mac, 0, IPA_MAC_ADDR_SIZE);
		inactive_lan_client_index[i].lan_stats_idx = -1;
		memset(inactive_lan_client_index[i].mac, 0, IPA_MAC_ADDR_SIZE);
	}
#endif
	/* ODU routing table initilization */
	if(ipa_if_cate == ODU_IF)
	{
		/* only do one time ioctl to odu-driver to infrom in router or bridge mode*/
		if (IPACM_Lan::odu_up != true)
		{
				m_fd_odu = open(IPACM_Iface::ipacmcfg->DEVICE_NAME_ODU, O_RDWR);
				if (0 == m_fd_odu)
				{
					IPACMERR("Failed opening %s.\n", IPACM_Iface::ipacmcfg->DEVICE_NAME_ODU);
					return ;
				}

				if(IPACM_Iface::ipacmcfg->ipacm_odu_router_mode == true)
				{
					ret = ioctl(m_fd_odu, ODU_BRIDGE_IOC_SET_MODE, ODU_BRIDGE_MODE_ROUTER);
					IPACM_Iface::ipacmcfg->ipacm_odu_enable = true;
				}
				else
				{
					ret = ioctl(m_fd_odu, ODU_BRIDGE_IOC_SET_MODE, ODU_BRIDGE_MODE_BRIDGE);
					IPACM_Iface::ipacmcfg->ipacm_odu_enable = true;
				}

				if (ret)
				{
					IPACMERR("Failed tell odu-driver the mode\n");
				}
				IPACMDBG("Tell odu-driver in router-mode(%d)\n", IPACM_Iface::ipacmcfg->ipacm_odu_router_mode);
				IPACMDBG_H("odu is up: odu-driver in router-mode(%d) \n", IPACM_Iface::ipacmcfg->ipacm_odu_router_mode);
				close(m_fd_odu);
				IPACM_Lan::odu_up = true;
		}
	}

	each_client_rt_rule_count[IPA_IP_v4] = 0;
	each_client_rt_rule_count[IPA_IP_v6] = 0;
	if(iface_query != NULL && tx_prop != NULL)
	{
		for(i=0; i<iface_query->num_tx_props; i++)
			each_client_rt_rule_count[tx_prop->tx[i].ip]++;
	}
	IPACMDBG_H("Need to add %d IPv4 and %d IPv6 routing rules for eth bridge for each client.\n", each_client_rt_rule_count[IPA_IP_v4], each_client_rt_rule_count[IPA_IP_v6]);

#ifdef FEATURE_IPA_ANDROID
	/* set the IPA-client pipe enum */
	if(ipa_if_cate == LAN_IF)
	{
		handle_tethering_client(false, IPACM_CLIENT_USB);
	}
#endif
	return;
}

IPACM_Lan::~IPACM_Lan()
{
	IPACM_EvtDispatcher::deregistr(this);
	IPACM_IfaceManager::deregistr(this);
	return;
}

/* LAN-iface's callback function */
void IPACM_Lan::event_callback(ipa_cm_event_id event, void *param)
{
	if(is_active == false && event != IPA_LAN_DELETE_SELF)
	{
		IPACMDBG_H("The interface is no longer active, return.\n");
		return;
	}

	int ipa_interface_index;
	ipacm_ext_prop* ext_prop;
	ipacm_event_iface_up* data_wan;
	ipacm_event_iface_up_tehter* data_wan_tether;
	int clnt_indx;

	switch (event)
	{
	case IPA_LINK_DOWN_EVENT:
		{
			ipacm_event_data_fid *data = (ipacm_event_data_fid *)param;
			ipa_interface_index = iface_ipa_index_query(data->if_index);
			if (ipa_interface_index == ipa_if_num)
			{
				IPACMDBG_H("Received IPA_LINK_DOWN_EVENT\n");
				handle_down_evt();
				IPACM_Iface::ipacmcfg->DelNatIfaces(dev_name); // delete NAT-iface
				return;
			}
		}
		break;

	case IPA_CFG_CHANGE_EVENT:
		{
			if ( IPACM_Iface::ipacmcfg->iface_table[ipa_if_num].if_cat != ipa_if_cate)
			{
				IPACMDBG_H("Received IPA_CFG_CHANGE_EVENT and category changed\n");
				/* delete previous instance */
				handle_down_evt();
				IPACM_Iface::ipacmcfg->DelNatIfaces(dev_name); // delete NAT-iface
				is_mode_switch = true; // need post internal usb-link up event
				return;
			}
			/* Add Natting iface to IPACM_Config if there is  Rx/Tx property */
			if (rx_prop != NULL || tx_prop != NULL)
			{
				IPACMDBG_H(" Has rx/tx properties registered for iface %s, add for NATTING \n", dev_name);
				IPACM_Iface::ipacmcfg->AddNatIfaces(dev_name);
			}
		}
		break;
#ifdef FEATURE_IPACM_UL_FIREWALL
	case IPA_FIREWALL_CHANGE_EVENT:
		IPACMDBG_H("Received IPA_FIREWALL_CHANGE_EVENT\n");

		if(ip_type != IPA_IP_v4)
		{
			IPACMDBG_H ("iface_ul_firewall Addr = (0x%x)\n", &iface_ul_firewall);
			re_config_dft_firewall_rules_ul(IPA_IP_v6, &iface_ul_firewall);
		}
		else
		{
			IPACMERR("IP type is not valid.\n");
		}
		break;
#endif
	case IPA_PRIVATE_SUBNET_CHANGE_EVENT:
		{
			ipacm_event_data_fid *data = (ipacm_event_data_fid *)param;
			/* internel event: data->if_index is ipa_if_index */
			if (data->if_index == ipa_if_num)
			{
				IPACMDBG_H("Received IPA_PRIVATE_SUBNET_CHANGE_EVENT from itself posting, ignore\n");
				return;
			}
			else
			{
				IPACMDBG_H("Received IPA_PRIVATE_SUBNET_CHANGE_EVENT from other LAN iface \n");
#ifdef FEATURE_IPA_ANDROID
				handle_private_subnet_android(IPA_IP_v4);
#endif
				IPACMDBG_H(" delete old private subnet rules, use new sets \n");
				return;
			}
		}
		break;

	case IPA_LAN_DELETE_SELF:
	{
		ipacm_event_data_fid *data = (ipacm_event_data_fid *)param;
		if(data->if_index == ipa_if_num)
		{
			IPACMDBG_H("Received IPA_LAN_DELETE_SELF event.\n");
			IPACMDBG_H("ipa_LAN (%s):ipa_index (%d) instance close \n", IPACM_Iface::ipacmcfg->iface_table[ipa_if_num].iface_name, ipa_if_num);
			/* posting link-up event for cradle use-case */
			if(is_mode_switch)
			{
				IPACMDBG_H("Posting IPA_USB_LINK_UP_EVENT event for (%s)\n", dev_name);
				ipacm_cmd_q_data evt_data;
				memset(&evt_data, 0, sizeof(evt_data));

				ipacm_event_data_fid *data_fid = NULL;
				data_fid = (ipacm_event_data_fid *)malloc(sizeof(ipacm_event_data_fid));
				if(data_fid == NULL)
				{
					IPACMERR("unable to allocate memory for IPA_USB_LINK_UP_EVENT data_fid\n");
					return;
				}
				if(IPACM_Iface::ipa_get_if_index(dev_name, &(data_fid->if_index)))
				{
					IPACMERR("Error while getting interface index for %s device", dev_name);
				}
				evt_data.event = IPA_USB_LINK_UP_EVENT;
				evt_data.evt_data = data_fid;
				//IPACMDBG_H("Posting event:%d\n", evt_data.event);
				IPACM_EvtDispatcher::PostEvt(&evt_data);
			}
#ifndef FEATURE_IPA_ANDROID
			if(rx_prop != NULL)
			{
				if(IPACM_Iface::ipacmcfg->getFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4) != 0)
				{
					IPACMDBG_DMESG("### WARNING ### num ipv4 flt rules on client %d is not expected: %d expected value: 0",
						rx_prop->rx[0].src_pipe, IPACM_Iface::ipacmcfg->getFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4));
				}
				if(IPACM_Iface::ipacmcfg->getFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6) != 0)
				{
					IPACMDBG_DMESG("### WARNING ### num ipv6 flt rules on client %d is not expected: %d expected value: 0",
						rx_prop->rx[0].src_pipe, IPACM_Iface::ipacmcfg->getFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6));
				}
			}
#endif
			delete this;
		}
		break;
	}

	case IPA_ADDR_ADD_EVENT:
		{
			ipacm_event_data_addr *data = (ipacm_event_data_addr *)param;
			ipa_interface_index = iface_ipa_index_query(data->if_index);

			if ( (data->iptype == IPA_IP_v4 && data->ipv4_addr == 0) ||
					 (data->iptype == IPA_IP_v6 &&
						data->ipv6_addr[0] == 0 && data->ipv6_addr[1] == 0 &&
					  data->ipv6_addr[2] == 0 && data->ipv6_addr[3] == 0) )
			{
				IPACMDBG_H("Invalid address, ignore IPA_ADDR_ADD_EVENT event\n");
				return;
			}
#ifdef FEATURE_L2TP
			if(data->iptype == IPA_IP_v6 && is_vlan_event(data->iface_name) && is_unique_local_ipv6_addr(data->ipv6_addr))
			{
				IPACMDBG_H("Got IPv6 new addr event for a vlan iface %s.\n", data->iface_name);
				eth_bridge_post_event(IPA_HANDLE_VLAN_IFACE_INFO, data->iptype, NULL,
					data->ipv6_addr, data->iface_name);
			}
#endif
			if (ipa_interface_index == ipa_if_num)
			{
				IPACMDBG_H("Received IPA_ADDR_ADD_EVENT\n");

				/* only call ioctl for ODU iface with bridge mode */
				if(IPACM_Iface::ipacmcfg->ipacm_odu_enable == true && IPACM_Iface::ipacmcfg->ipacm_odu_router_mode == false
						&& ipa_if_cate == ODU_IF)
				{
					if((data->iptype == IPA_IP_v6) && (num_dft_rt_v6 == 0))
					{
						handle_addr_evt_odu_bridge(data);
					}
#ifdef FEATURE_IPA_ANDROID
					add_dummy_private_subnet_flt_rule(data->iptype);
					handle_private_subnet_android(data->iptype);
#else
					handle_private_subnet(data->iptype);
#endif
				}
				else
				{

					/* check v4 not setup before, v6 can have 2 iface ip */
					if( ((data->iptype != ip_type) && (ip_type != IPA_IP_MAX))
						|| ((data->iptype==IPA_IP_v6) && (num_dft_rt_v6!=MAX_DEFAULT_v6_ROUTE_RULES)))
					{
						IPACMDBG_H("Got IPA_ADDR_ADD_EVENT ip-family:%d, v6 num %d: \n",data->iptype,num_dft_rt_v6);
						if(handle_addr_evt(data) == IPACM_FAILURE)
						{
							return;
						}

#ifdef FEATURE_IPA_ANDROID
						add_dummy_private_subnet_flt_rule(data->iptype);
						handle_private_subnet_android(data->iptype);
#else
						handle_private_subnet(data->iptype);
#endif

						if (IPACM_Wan::isWanUP(ipa_if_num))
						{
							if(data->iptype == IPA_IP_v4 || data->iptype == IPA_IP_MAX)
							{
								if(IPACM_Wan::backhaul_is_sta_mode == false)
								{
									ext_prop = IPACM_Iface::ipacmcfg->GetExtProp(IPA_IP_v4);
									handle_wan_up_ex(ext_prop, IPA_IP_v4,
												IPACM_Wan::getXlat_Mux_Id());
								}
								else
								{
									handle_wan_up(IPA_IP_v4);
								}
							}
						}
#ifdef FEATURE_IPACM_UL_FIREWALL
						IPACM_Wan::read_firewall_filter_rules_ul();
#endif
						if(IPACM_Wan::isWanUP_V6(ipa_if_num)) /* Modem v6 call is UP?*/
						{
#ifdef FEATURE_IPACM_UL_FIREWALL
							IPACMDBG_H("LTE BH UP\n");
							if (IPACM_Wan::firewall_config_ul.rule_action_accept == true) /* is whitelist ?? */
							{	/* Configure and send the firewall filter table to Q6*/
								delete_uplink_filter_rule_ul(data->iptype, &iface_ul_firewall);

								/* Configure and send the firewall filter table to Q6*/
								IPACM_Lan::config_dft_firewall_rules_ul_ex(IPACM_Wan::firewall_flt_rule_v6_ul, data->iptype);
								IPACM_Lan::config_wan_frag_firewall_rule_ul_ex(true, data->iptype, &iface_ul_firewall);
								IPACMDBG_H ("New config rules sent to Q6\n");
							}
							else
							{	/* Config and install it on pipes directly, since it is Blacklisted */
								IPACMDBG_H ("Send indication to Q6 to disable UL firewall\n");
								IPACM_Lan::config_wan_frag_firewall_rule_ul_ex(false, data->iptype, &iface_ul_firewall);
								install_wan_firewall_rule_ul(false, data->iptype);

								IPACM_Lan::config_dft_firewall_rules_ul(IPACM_Wan::firewall_flt_rule_v6_ul, data->iptype, &iface_ul_firewall);
								if(rx_prop) {
									IPACMDBG_H ("Pipe (%d) configured with the new UL rules\n", rx_prop->rx[0].src_pipe);
								}
							}
#endif //FEATURE_IPACM_UL_FIREWALL
							if((data->iptype == IPA_IP_v6 || data->iptype == IPA_IP_MAX) && num_dft_rt_v6 == 1)
							{
								memcpy(ipv6_prefix, IPACM_Wan::backhaul_ipv6_prefix, sizeof(ipv6_prefix));
								install_ipv6_prefix_flt_rule(IPACM_Wan::backhaul_ipv6_prefix);
								if(IPACM_Wan::backhaul_is_sta_mode == false)
								{
									ext_prop = IPACM_Iface::ipacmcfg->GetExtProp(IPA_IP_v6);
									handle_wan_up_ex(ext_prop, IPA_IP_v6, 0);
								}
								else
								{
									handle_wan_up(IPA_IP_v6);
								}
							}
						}
#ifdef FEATURE_IPACM_UL_FIREWALL
						else
						{
							IPACMDBG_H("NON - LTE BH UP\n");
							/* Config and install it on pipes directly, Other backhaul case*/
							IPACMDBG_H ("Send indication to Q6 to disable UL firewall\n");
							IPACM_Lan::config_wan_frag_firewall_rule_ul_ex(false, data->iptype, &iface_ul_firewall); /* Deleting frag filters rules installed on LTE BH Whitelisting */
							install_wan_firewall_rule_ul(false, data->iptype);

							IPACM_Lan::config_dft_firewall_rules_ul(IPACM_Wan::firewall_flt_rule_v6_ul, data->iptype, &iface_ul_firewall);
							if(rx_prop) {
								IPACMDBG_H ("Pipe (%d) configured with the new UL rules\n", rx_prop->rx[0].src_pipe);
							}
						}
#endif //FEATURE_IPACM_UL_FIREWALL
						/* Post event to NAT */
						if (data->iptype == IPA_IP_v4)
						{
							ipacm_cmd_q_data evt_data;
							ipacm_event_iface_up *info;

							info = (ipacm_event_iface_up *)
								malloc(sizeof(ipacm_event_iface_up));
							if (info == NULL)
							{
								IPACMERR("Unable to allocate memory\n");
								return;
							}

							memcpy(info->ifname, dev_name, IF_NAME_LEN);
							info->ipv4_addr = data->ipv4_addr;
							info->addr_mask = IPACM_Iface::ipacmcfg->private_subnet_table[0].subnet_mask;

							evt_data.event = IPA_HANDLE_LAN_UP;
							evt_data.evt_data = (void *)info;

							/* Insert IPA_HANDLE_LAN_UP to command queue */
							IPACMDBG_H("posting IPA_HANDLE_LAN_UP for IPv4 with below information\n");
							IPACMDBG_H("IPv4 address:0x%x, IPv4 address mask:0x%x\n",
											info->ipv4_addr, info->addr_mask);
							IPACM_EvtDispatcher::PostEvt(&evt_data);
						}
						IPACMDBG_H("Finish handling IPA_ADDR_ADD_EVENT for ip-family(%d)\n", data->iptype);
					}

					IPACMDBG_H("Finish handling IPA_ADDR_ADD_EVENT for ip-family(%d)\n", data->iptype);
					/* checking if SW-RT_enable */
					if (IPACM_Iface::ipacmcfg->ipa_sw_rt_enable == true)
					{
						/* handle software routing enable event*/
						IPACMDBG_H("IPA_SW_ROUTING_ENABLE for iface: %s \n",IPACM_Iface::ipacmcfg->iface_table[ipa_if_num].iface_name);
						handle_software_routing_enable();
					}

				}
			}
		}
		break;
#ifdef FEATURE_IPA_ANDROID
	case IPA_HANDLE_WAN_UP_TETHER:
		IPACMDBG_H("Received IPA_HANDLE_WAN_UP_TETHER event\n");

		data_wan_tether = (ipacm_event_iface_up_tehter*)param;
		if(data_wan_tether == NULL)
		{
			IPACMERR("No event data is found.\n");
			return;
		}
		IPACMDBG_H("Backhaul is sta mode?%d, if_index_tether:%d tether_if_name:%s\n", data_wan_tether->is_sta,
					data_wan_tether->if_index_tether,
					IPACM_Iface::ipacmcfg->iface_table[data_wan_tether->if_index_tether].iface_name);
		if (data_wan_tether->if_index_tether == ipa_if_num)
		{
			if(ip_type == IPA_IP_v4 || ip_type == IPA_IP_MAX)
			{
				if(data_wan_tether->is_sta == false)
				{
					ext_prop = IPACM_Iface::ipacmcfg->GetExtProp(IPA_IP_v4);
					handle_wan_up_ex(ext_prop, IPA_IP_v4, 0);
				}
				else
				{
					handle_wan_up(IPA_IP_v4);
				}
			}
		}
		break;

	case IPA_HANDLE_WAN_UP_V6_TETHER:
		IPACMDBG_H("Received IPA_HANDLE_WAN_UP_V6_TETHER event\n");

		data_wan_tether = (ipacm_event_iface_up_tehter*)param;
		if(data_wan_tether == NULL)
		{
			IPACMERR("No event data is found.\n");
			return;
		}
		IPACMDBG_H("Backhaul is sta mode?%d, if_index_tether:%d tether_if_name:%s\n", data_wan_tether->is_sta,
					data_wan_tether->if_index_tether,
					IPACM_Iface::ipacmcfg->iface_table[data_wan_tether->if_index_tether].iface_name);
		if (data_wan_tether->if_index_tether == ipa_if_num)
		{
			if(ip_type == IPA_IP_v6 || ip_type == IPA_IP_MAX)
			{
					memcpy(ipv6_prefix, data_wan_tether->ipv6_prefix, sizeof(ipv6_prefix));
					install_ipv6_prefix_flt_rule(data_wan_tether->ipv6_prefix);
					if(data_wan_tether->is_sta == false)
					{
						ext_prop = IPACM_Iface::ipacmcfg->GetExtProp(IPA_IP_v6);
						handle_wan_up_ex(ext_prop, IPA_IP_v6, 0);
					}
					else
					{
						handle_wan_up(IPA_IP_v6);
					}
			}
		}
		break;

	case IPA_HANDLE_WAN_DOWN_TETHER:
		IPACMDBG_H("Received IPA_HANDLE_WAN_DOWN_TETHER event\n");
		data_wan_tether = (ipacm_event_iface_up_tehter*)param;
		if(data_wan_tether == NULL)
		{
			IPACMERR("No event data is found.\n");
			return;
		}
		IPACMDBG_H("Backhaul is sta mode?%d, if_index_tether:%d tether_if_name:%s\n", data_wan_tether->is_sta,
					data_wan_tether->if_index_tether,
					IPACM_Iface::ipacmcfg->iface_table[data_wan_tether->if_index_tether].iface_name);
		if (data_wan_tether->if_index_tether == ipa_if_num)
		{
			if(ip_type == IPA_IP_v4 || ip_type == IPA_IP_MAX)
			{
				handle_wan_down(data_wan_tether->is_sta);
			}
		}
		break;

	case IPA_HANDLE_WAN_DOWN_V6_TETHER:
		IPACMDBG_H("Received IPA_HANDLE_WAN_DOWN_V6_TETHER event\n");
		data_wan_tether = (ipacm_event_iface_up_tehter*)param;
		if(data_wan_tether == NULL)
		{
			IPACMERR("No event data is found.\n");
			return;
		}
		IPACMDBG_H("Backhaul is sta mode?%d, if_index_tether:%d tether_if_name:%s\n", data_wan_tether->is_sta,
					data_wan_tether->if_index_tether,
					IPACM_Iface::ipacmcfg->iface_table[data_wan_tether->if_index_tether].iface_name);
		if (data_wan_tether->if_index_tether == ipa_if_num)
		{
			/* clean up v6 RT rules*/
			IPACMDBG_H("Received IPA_HANDLE_WAN_DOWN_V6_TETHER in LAN-instance and need clean up client IPv6 address \n");
			/* reset usb-client ipv6 rt-rules */
			handle_lan_client_reset_rt(IPA_IP_v6);

			if(ip_type == IPA_IP_v6 || ip_type == IPA_IP_MAX)
			{
				handle_wan_down_v6(data_wan_tether->is_sta);
			}
		}
		break;
#else
	case IPA_HANDLE_WAN_UP:
		IPACMDBG_H("Received IPA_HANDLE_WAN_UP event\n");

		data_wan = (ipacm_event_iface_up*)param;
		if(data_wan == NULL)
		{
			IPACMERR("No event data is found.\n");
			return;
		}
		IPACMDBG_H("Backhaul is sta mode?%d\n", data_wan->is_sta);
		if(ip_type == IPA_IP_v4 || ip_type == IPA_IP_MAX)
		{
		if(data_wan->is_sta == false)
		{
				ext_prop = IPACM_Iface::ipacmcfg->GetExtProp(IPA_IP_v4);
				handle_wan_up_ex(ext_prop, IPA_IP_v4, data_wan->xlat_mux_id);
		}
		else
		{
			handle_wan_up(IPA_IP_v4);
		}
		}
		break;

	case IPA_HANDLE_WAN_UP_V6:
		IPACMDBG_H("Received IPA_HANDLE_WAN_UP_V6 event\n");

		data_wan = (ipacm_event_iface_up*)param;
		if(data_wan == NULL)
		{
			IPACMERR("No event data is found.\n");
			return;
		}
		IPACMDBG_H("Backhaul is sta mode?%d\n", data_wan->is_sta);
		if(ip_type == IPA_IP_v6 || ip_type == IPA_IP_MAX)
		{
#ifdef FEATURE_IPACM_UL_FIREWALL
			IPACM_Wan::read_firewall_filter_rules_ul();
			if(IPACM_Wan::isWanUP_V6(ipa_if_num))
			{
				IPACMDBG_H("LTE BH UP\n");
				if (IPACM_Wan::firewall_config_ul.rule_action_accept == true) /* is whitelist ?? */
				{	/* Configure and send the firewall filter table to Q6*/
					delete_uplink_filter_rule_ul(ip_type, &iface_ul_firewall);

					/* Configure and send the firewall filter table to Q6*/
					IPACM_Lan::config_dft_firewall_rules_ul_ex(IPACM_Wan::firewall_flt_rule_v6_ul, ip_type);
					IPACM_Lan::config_wan_frag_firewall_rule_ul_ex(true, ip_type, &iface_ul_firewall);
					IPACMDBG_H ("New config rules sent to Q6\n");
				}
				else
				{	/* Config and install it on pipes directly, since it is Blacklisted */
					IPACMDBG_H ("Send indication to Q6 to disable UL firewall\n");
					IPACM_Lan::config_wan_frag_firewall_rule_ul_ex(false, ip_type, &iface_ul_firewall);
					install_wan_firewall_rule_ul(false, ip_type);

					IPACM_Lan::config_dft_firewall_rules_ul(IPACM_Wan::firewall_flt_rule_v6_ul, ip_type, &iface_ul_firewall);
					if(rx_prop) {
						IPACMDBG_H ("Pipe (%d) configured with the new UL rules\n", rx_prop->rx[0].src_pipe);
					}
				}
			}
			else
			{
				IPACMDBG_H("NON - LTE BH UP\n");
				/* Config and install it on pipes directly, Other backhaul case*/
				IPACMDBG_H ("Send indication to Q6 to disable UL firewall\n");
				IPACM_Lan::config_wan_frag_firewall_rule_ul_ex(false, ip_type, &iface_ul_firewall);
				install_wan_firewall_rule_ul(false, ip_type);

				IPACM_Lan::config_dft_firewall_rules_ul(IPACM_Wan::firewall_flt_rule_v6_ul, ip_type, &iface_ul_firewall);
				if(rx_prop) {
					IPACMDBG_H ("Pipe (%d) configured with the new UL rules\n", rx_prop->rx[0].src_pipe);
				}
			}
#endif //FEATURE_IPACM_UL_FIREWALL
			memcpy(ipv6_prefix, data_wan->ipv6_prefix, sizeof(ipv6_prefix));
			install_ipv6_prefix_flt_rule(data_wan->ipv6_prefix);
			if(data_wan->is_sta == false)
			{
				ext_prop = IPACM_Iface::ipacmcfg->GetExtProp(IPA_IP_v6);
				handle_wan_up_ex(ext_prop, IPA_IP_v6, 0);
			}
			else
			{
				handle_wan_up(IPA_IP_v6);
			}
		}
		break;

	case IPA_HANDLE_WAN_DOWN:
		IPACMDBG_H("Received IPA_HANDLE_WAN_DOWN event\n");
		data_wan = (ipacm_event_iface_up*)param;
		if(data_wan == NULL)
		{
			IPACMERR("No event data is found.\n");
			return;
		}
		IPACMDBG_H("Backhaul is sta mode?%d\n", data_wan->is_sta);
		if(ip_type == IPA_IP_v4 || ip_type == IPA_IP_MAX)
		{
			handle_wan_down(data_wan->is_sta);
		}
		break;

	case IPA_HANDLE_WAN_DOWN_V6:
		IPACMDBG_H("Received IPA_HANDLE_WAN_DOWN_V6 event\n");
		data_wan = (ipacm_event_iface_up*)param;
		if(data_wan == NULL)
		{
			IPACMERR("No event data is found.\n");
			return;
		}
		/* clean up v6 RT rules*/
		IPACMDBG_H("Received IPA_WAN_V6_DOWN in LAN-instance and need clean up client IPv6 address \n");
		/* reset usb-client ipv6 rt-rules */
		handle_lan_client_reset_rt(IPA_IP_v6);

		IPACMDBG_H("Backhaul is sta mode?%d\n", data_wan->is_sta);
		if(ip_type == IPA_IP_v6 || ip_type == IPA_IP_MAX)
		{
			handle_wan_down_v6(data_wan->is_sta);
		}
		break;
#endif

	case IPA_NEIGH_CLIENT_IP_ADDR_ADD_EVENT:
		{
			ipacm_event_data_all *data = (ipacm_event_data_all *)param;
			ipa_interface_index = iface_ipa_index_query(data->if_index);
			IPACMDBG_H("Recieved IPA_NEIGH_CLIENT_IP_ADDR_ADD_EVENT event \n");
			IPACMDBG_H("check iface %s category: %d\n", dev_name, ipa_if_cate);
			if (ipa_interface_index == ipa_if_num && ipa_if_cate == ODU_IF)
			{
				IPACMDBG_H("ODU iface got v4-ip \n");
				/* first construc ODU full header */
				if ((ipv4_header_set == false) && (ipv6_header_set == false))
				{
					/* construct ODU RT tbl */
					handle_odu_hdr_init(data->mac_addr);
					if (IPACM_Iface::ipacmcfg->ipacm_odu_embms_enable == true)
					{
						handle_odu_route_add();
						IPACMDBG_H("construct ODU header and route rules, embms_flag (%d) \n", IPACM_Iface::ipacmcfg->ipacm_odu_embms_enable);
					}
					else
					{
						IPACMDBG_H("construct ODU header only, embms_flag (%d) \n", IPACM_Iface::ipacmcfg->ipacm_odu_embms_enable);
					}
				}
				/* if ODU in bridge mode, directly return */
				if(IPACM_Iface::ipacmcfg->ipacm_odu_router_mode == false)
				{
					IPACMDBG_H("ODU is in bridge mode, no action \n");
					return;
				}
			}

			if (ipa_interface_index == ipa_if_num || is_vlan_event(data->iface_name)
				|| (is_l2tp_event(data->iface_name) && ipa_if_cate == ODU_IF))
			{
				IPACMDBG_H("ETH iface got client \n");
				if(ipa_interface_index == ipa_if_num)
				{
					/* first construc ETH full header */
					handle_eth_hdr_init(data->mac_addr);
					IPACMDBG_H("construct ETH header and route rules \n");
					/* Associate with IP and construct RT-rule */
					if (handle_eth_client_ipaddr(data) == IPACM_FAILURE)
					{
						return;
					}
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
					if (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable == false)
#endif
					{
						handle_eth_client_route_rule(data->mac_addr, data->iptype);
						if (data->iptype == IPA_IP_v4)
						{
							/* Add NAT rules after ipv4 RT rules are set */
							CtList->HandleNeighIpAddrAddEvt(data);
						}
					}
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
					else
					{
						handle_eth_client_route_rule_ext(data->mac_addr, data->iptype);
					}
#endif
					eth_bridge_post_event(IPA_ETH_BRIDGE_CLIENT_ADD, IPA_IP_MAX, data->mac_addr, NULL, data->iface_name);
				}
#ifdef FEATURE_L2TP
				else if(is_l2tp_event(data->iface_name) && ipa_if_cate == ODU_IF)
				{
					if(tx_prop != NULL)
					{
						IPACMDBG_H("add rm dependency for L2TP interface.\n");
						IPACM_Iface::ipacmcfg->AddRmDepend(IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[tx_prop->tx[0].dst_pipe],false);
					}
					eth_bridge_post_event(IPA_ETH_BRIDGE_CLIENT_ADD, IPA_IP_MAX, data->mac_addr, NULL, data->iface_name);
				}
				else
				{
					if(data->iptype == IPA_IP_v6 && is_unique_local_ipv6_addr(data->ipv6_addr))
					{
						eth_bridge_post_event(IPA_HANDLE_VLAN_CLIENT_INFO, IPA_IP_MAX, data->mac_addr, data->ipv6_addr, data->iface_name);
					}
				}
#endif
				return;
			}
		}
		break;

	case IPA_NEIGH_CLIENT_IP_ADDR_DEL_EVENT:
		{
			ipacm_event_data_all *data = (ipacm_event_data_all *)param;
			ipa_interface_index = iface_ipa_index_query(data->if_index);

			IPACMDBG_H("Received IPA_NEIGH_CLIENT_IP_ADDR_DEL_EVENT event. \n");
			IPACMDBG_H("check iface %s category: %d\n", dev_name, ipa_if_cate);
			/* if ODU in bridge mode, directly return */
			if (ipa_if_cate == ODU_IF && IPACM_Iface::ipacmcfg->ipacm_odu_router_mode == false)
			{
				IPACMDBG_H("ODU is in bridge mode, no action \n");
				return;
			}

			if (ipa_interface_index == ipa_if_num
				|| (is_l2tp_event(data->iface_name) && ipa_if_cate == ODU_IF))
			{
				if(ipa_interface_index == ipa_if_num)
				{
					if (data->iptype == IPA_IP_v6)
					{
						handle_del_ipv6_addr(data);
						return;
					}
					IPACMDBG_H("LAN iface delete client \n");
					handle_eth_client_down_evt(data->mac_addr);
				}
				else
				{
					eth_bridge_post_event(IPA_ETH_BRIDGE_CLIENT_DEL, IPA_IP_MAX, data->mac_addr, NULL, data->iface_name);
				}
				return;
			}
		}
		break;

	case IPA_SW_ROUTING_ENABLE:
		IPACMDBG_H("Received IPA_SW_ROUTING_ENABLE\n");
		/* handle software routing enable event*/
		handle_software_routing_enable();
		break;

	case IPA_SW_ROUTING_DISABLE:
		IPACMDBG_H("Received IPA_SW_ROUTING_DISABLE\n");
		/* handle software routing disable event*/
		handle_software_routing_disable();
		break;

	case IPA_CRADLE_WAN_MODE_SWITCH:
	{
		IPACMDBG_H("Received IPA_CRADLE_WAN_MODE_SWITCH event.\n");
		ipacm_event_cradle_wan_mode* wan_mode = (ipacm_event_cradle_wan_mode*)param;
		if(wan_mode == NULL)
		{
			IPACMERR("Event data is empty.\n");
			return;
		}

		if(wan_mode->cradle_wan_mode == BRIDGE)
		{
			handle_cradle_wan_mode_switch(true);
		}
		else
		{
			handle_cradle_wan_mode_switch(false);
		}
	}
	break;

	case IPA_TETHERING_STATS_UPDATE_EVENT:
	{
		IPACMDBG_H("Received IPA_TETHERING_STATS_UPDATE_EVENT event.\n");
		if (IPACM_Wan::isWanUP(ipa_if_num) || IPACM_Wan::isWanUP_V6(ipa_if_num))
		{
			if(IPACM_Wan::backhaul_is_sta_mode == false) /* LTE */
			{
				ipa_get_data_stats_resp_msg_v01 *data = (ipa_get_data_stats_resp_msg_v01 *)param;
				IPACMDBG("Received IPA_TETHERING_STATS_UPDATE_STATS ipa_stats_type: %d\n",data->ipa_stats_type);
				IPACMDBG("Received %d UL, %d DL pipe stats\n",data->ul_src_pipe_stats_list_len,
					data->dl_dst_pipe_stats_list_len);
				if (data->ipa_stats_type != QMI_IPA_STATS_TYPE_PIPE_V01)
				{
					IPACMERR("not valid pipe stats enum(%d)\n", data->ipa_stats_type);
					return;
				}
				handle_tethering_stats_event(data);
			}
		}
	}
	break;

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	/* QCMAP sends this event whenever a client is connected. */
	case IPA_LAN_CLIENT_CONNECT_EVENT:
		{
			ipacm_event_data_mac *data = (ipacm_event_data_mac *)param;
			if (!IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
			{
				IPACMDBG_H("LAN stats functionality is not enabled, ignore IPA_LAN_CLIENT_CONNECT_EVENT.\n");
				return;
			}
			ipa_interface_index = iface_ipa_index_query(data->if_index);
			if (ipa_interface_index == ipa_if_num)
			{
				IPACMDBG_H("Received IPA_LAN_CLIENT_CONNECT_EVENT\n");
				/* Check if we can add this to the active list. */
				/* Active List:- Clients for which index is less than IPA_MAX_NUM_HW_PATH_CLIENTS. */
				if (get_free_active_lan_stats_index(data->mac_addr) == -1)
				{
					IPACMDBG_H("Failed to reserve active lan_stats index, try inactive list. \n");
					/* Try to get the inactive index which can be used later. */
					if (get_free_inactive_lan_stats_index(data->mac_addr) == -1)
					{
						IPACMDBG_H("Failed to reserve inactive lan_stats index, return\n");
					}
					return;
				}
				/* Check if the client is inactive list and remove it*/
				if (reset_inactive_lan_stats_index(data->mac_addr) == -1)
				{
					IPACMDBG_H("Failed to reset inactive lan_stats index, return\n");
				}
				/* Check if the client is already initialized and add filter/routing rules. */
				IPACM_Lan::handle_lan_client_connect(data->mac_addr);
			}
		}
		break;
	/* QCMAP sends this event whenever a client is disconnected. */
	case IPA_LAN_CLIENT_DISCONNECT_EVENT:
		{
			ipacm_event_data_mac *data = (ipacm_event_data_mac *)param;
			if (!IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
			{
				IPACMDBG_H("LAN stats functionality is not enabled, ignore IPA_LAN_CLIENT_DISCONNECT_EVENT.\n");
				return;
			}
			ipa_interface_index = iface_ipa_index_query(data->if_index);
			if (ipa_interface_index == ipa_if_num)
			{
				IPACMDBG_H("Received IPA_LAN_CLIENT_DISCONNECT_EVENT\n");
				IPACM_Lan::handle_lan_client_disconnect(data->mac_addr);
			}
		}
		break;
#endif

	default:
		break;
	}

	return;
}


int IPACM_Lan::handle_del_ipv6_addr(ipacm_event_data_all *data)
{
	uint32_t tx_index;
	uint32_t rt_hdl;
	int num_v6 =0, clnt_indx;

	clnt_indx = get_eth_client_index(data->mac_addr);
	if (clnt_indx == IPACM_INVALID_INDEX)
	{
		IPACMERR("eth client not found/attached \n");
		return IPACM_FAILURE;
	}

	if(data->iptype == IPA_IP_v6)
	{
		if ((data->ipv6_addr[0] != 0) || (data->ipv6_addr[1] != 0) ||
				(data->ipv6_addr[2] != 0) || (data->ipv6_addr[3] || 0))
		{
			IPACMDBG_H("ipv6 address got: 0x%x:%x:%x:%x\n", data->ipv6_addr[0], data->ipv6_addr[1], data->ipv6_addr[2], data->ipv6_addr[3]);
			for(num_v6=0;num_v6 < get_client_memptr(eth_client, clnt_indx)->ipv6_set;num_v6++)
			{
				if( data->ipv6_addr[0] == get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6][0] &&
					data->ipv6_addr[1] == get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6][1] &&
					data->ipv6_addr[2]== get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6][2] &&
					data->ipv6_addr[3] == get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6][3])
				{
					IPACMDBG_H("ipv6 addr is found at position:%d for client:%d\n", num_v6, clnt_indx);
					break;
				}
			}
		}
		if (num_v6 == IPV6_NUM_ADDR)
		{
			IPACMDBG_H("ipv6 addr is not found. \n");
			return IPACM_FAILURE;
		}

		for(tx_index = 0; tx_index < iface_query->num_tx_props; tx_index++)
		{
			if((tx_prop->tx[tx_index].ip == IPA_IP_v6) && (get_client_memptr(eth_client, clnt_indx)->route_rule_set_v6 != 0))
			{
				IPACMDBG_H("Delete client index %d ipv6 RT-rules for %d-st ipv6 for tx:%d\n", clnt_indx, num_v6, tx_index);
				rt_hdl = get_client_memptr(eth_client, clnt_indx)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6[num_v6];
				if(m_routing.DeleteRoutingHdl(rt_hdl, IPA_IP_v6) == false)
				{
					return IPACM_FAILURE;
				}
				rt_hdl = get_client_memptr(eth_client, clnt_indx)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6_wan[num_v6];
				if(m_routing.DeleteRoutingHdl(rt_hdl, IPA_IP_v6) == false)
				{
					return IPACM_FAILURE;
				}
				get_client_memptr(eth_client, clnt_indx)->ipv6_set--;
				get_client_memptr(eth_client, clnt_indx)->route_rule_set_v6--;

				for(num_v6;num_v6< get_client_memptr(eth_client, clnt_indx)->ipv6_set;num_v6++)
				{
					get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6][0] =
						get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6+1][0];
					get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6][1] =
						get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6+1][1];
					get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6][2] =
						get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6+1][2];
					get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6][3] =
						get_client_memptr(eth_client, clnt_indx)->v6_addr[num_v6+1][3];
					get_client_memptr(eth_client, clnt_indx)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6[num_v6] =
						get_client_memptr(eth_client, clnt_indx)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6[num_v6+1];
					get_client_memptr(eth_client, clnt_indx)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6_wan[num_v6] =
						get_client_memptr(eth_client, clnt_indx)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6_wan[num_v6+1];
				}
			}
		}
	}
	return IPACM_SUCCESS;
}

/* delete filter rule for wan_down event for IPv4*/
int IPACM_Lan::handle_wan_down(bool is_sta_mode)
{
	ipa_fltr_installed_notif_req_msg_v01 flt_index;
	int fd;

	fd = open(IPA_DEVICE_NAME, O_RDWR);
	if (0 == fd)
	{
		IPACMERR("Failed opening %s.\n", IPA_DEVICE_NAME);
		return IPACM_FAILURE;
	}

	if(is_sta_mode == false)
	{
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
		if (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable == false)
#endif
		{
			if (num_wan_ul_fl_rule_v4 > MAX_WAN_UL_FILTER_RULES)
			{
				IPACMERR("number of wan_ul_fl_rule_v4 (%d) > MAX_WAN_UL_FILTER_RULES (%d), aborting...\n", num_wan_ul_fl_rule_v4, MAX_WAN_UL_FILTER_RULES);
				close(fd);
				return IPACM_FAILURE;
			}
			if (m_filtering.DeleteFilteringHdls(wan_ul_fl_rule_hdl_v4,
				IPA_IP_v4, num_wan_ul_fl_rule_v4) == false)
			{
				IPACMERR("Error Deleting RuleTable(1) to Filtering, aborting...\n");
				close(fd);
				return IPACM_FAILURE;
			}
			IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4, num_wan_ul_fl_rule_v4);

			memset(wan_ul_fl_rule_hdl_v4, 0, MAX_WAN_UL_FILTER_RULES * sizeof(uint32_t));
			num_wan_ul_fl_rule_v4 = 0;
			modem_ul_v4_set = false;
		}
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
		else {
			if (delete_uplink_filter_rule(IPA_IP_v4) == IPACM_FAILURE)
			{
				IPACMERR("Error delete_uplink_filter_rule, aborting...\n");
				close(fd);
				return IPACM_FAILURE;
			}
			num_wan_ul_fl_rule_v4 = 0;
		}
#endif
		memset(&flt_index, 0, sizeof(flt_index));
		flt_index.source_pipe_index = ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, rx_prop->rx[0].src_pipe);
		flt_index.install_status = IPA_QMI_RESULT_SUCCESS_V01;
#ifndef FEATURE_IPA_V3
		flt_index.filter_index_list_len = 0;
#else /* defined (FEATURE_IPA_V3) */
		flt_index.rule_id_valid = 1;
		flt_index.rule_id_len = 0;
#endif
		flt_index.embedded_pipe_index_valid = 1;
		flt_index.embedded_pipe_index = ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, IPA_CLIENT_APPS_LAN_WAN_PROD);
		flt_index.retain_header_valid = 1;
		flt_index.retain_header = 0;
		flt_index.embedded_call_mux_id_valid = 1;
		flt_index.embedded_call_mux_id = IPACM_Iface::ipacmcfg->GetQmapId();

		if(false == m_filtering.SendFilteringRuleIndex(&flt_index))
		{
			IPACMERR("Error sending filtering rule index, aborting...\n");
			close(fd);
			return IPACM_FAILURE;
		}
	} else {
			if (m_filtering.DeleteFilteringHdls(&lan_wan_fl_rule_hdl[0], IPA_IP_v4, 1) == false)
			{
			IPACMERR("Error Adding RuleTable(1) to Filtering, aborting...\n");
			close(fd);
			return IPACM_FAILURE;
		}
		IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4, 1);
	}

	close(fd);
	return IPACM_SUCCESS;
}

/* handle new_address event*/
int IPACM_Lan::handle_addr_evt(ipacm_event_data_addr *data)
{
	struct ipa_ioc_add_rt_rule *rt_rule;
	struct ipa_rt_rule_add *rt_rule_entry;
	const int NUM_RULES = 1;
	int num_ipv6_addr;
	int res = IPACM_SUCCESS;

	IPACMDBG_H("set route/filter rule ip-type: %d \n", data->iptype);

/* Add private subnet*/
#ifdef FEATURE_IPA_ANDROID
	if (data->iptype == IPA_IP_v4)
	{
		IPACMDBG_H("current IPACM private subnet_addr number(%d)\n", IPACM_Iface::ipacmcfg->ipa_num_private_subnet);
		if_ipv4_subnet = (data->ipv4_addr >> 8) << 8;
		IPACMDBG_H(" Add IPACM private subnet_addr as: 0x%x \n", if_ipv4_subnet);
		if(IPACM_Iface::ipacmcfg->AddPrivateSubnet(if_ipv4_subnet, ipa_if_num) == false)
		{
			IPACMERR(" can't Add IPACM private subnet_addr as: 0x%x \n", if_ipv4_subnet);
		}
	}
#endif /* defined(FEATURE_IPA_ANDROID)*/

	/* Update the IP Type. */
	config_ip_type(data->iptype);

	if (data->iptype == IPA_IP_v4)
	{
		rt_rule = (struct ipa_ioc_add_rt_rule *)
			 calloc(1, sizeof(struct ipa_ioc_add_rt_rule) +
							NUM_RULES * sizeof(struct ipa_rt_rule_add));

		if (!rt_rule)
		{
			IPACMERR("Error Locate ipa_ioc_add_rt_rule memory...\n");
			return IPACM_FAILURE;
		}

		rt_rule->commit = 1;
		rt_rule->num_rules = NUM_RULES;
		rt_rule->ip = data->iptype;
		rt_rule_entry = &rt_rule->rules[0];
		rt_rule_entry->at_rear = false;
		rt_rule_entry->rule.dst = IPA_CLIENT_APPS_LAN_CONS;  //go to A5
		rt_rule_entry->rule.attrib.attrib_mask = IPA_FLT_DST_ADDR;
		strlcpy(rt_rule->rt_tbl_name, IPACM_Iface::ipacmcfg->rt_tbl_lan_v4.name, sizeof(rt_rule->rt_tbl_name));
		rt_rule_entry->rule.attrib.u.v4.dst_addr      = data->ipv4_addr;
		rt_rule_entry->rule.attrib.u.v4.dst_addr_mask = 0xFFFFFFFF;
#ifdef FEATURE_IPA_V3
		rt_rule_entry->rule.hashable = true;
#endif
		if (false == m_routing.AddRoutingRule(rt_rule))
		{
			IPACMERR("Routing rule addition failed!\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		else if (rt_rule_entry->status)
		{
			IPACMERR("rt rule adding failed. Result=%d\n", rt_rule_entry->status);
			res = rt_rule_entry->status;
			goto fail;
		}
		dft_rt_rule_hdl[0] = rt_rule_entry->rt_rule_hdl;
		IPACMDBG_H("ipv4 iface rt-rule hdl1=0x%x\n", dft_rt_rule_hdl[0]);
		/* initial multicast/broadcast/fragment filter rule */

		init_fl_rule(data->iptype);
#ifdef FEATURE_L2TP
		if(ipa_if_cate == WLAN_IF)
		{
			add_tcp_syn_flt_rule(data->iptype);
		}
#endif
		install_ipv4_icmp_flt_rule();

		/* populate the flt rule offset for eth bridge */
		eth_bridge_flt_rule_offset[data->iptype] = ipv4_icmp_flt_rule_hdl[0];
		eth_bridge_post_event(IPA_ETH_BRIDGE_IFACE_UP, IPA_IP_v4, NULL, NULL, NULL);
	}
	else
	{
	    /* check if see that v6-addr already or not*/
	    for(num_ipv6_addr=0;num_ipv6_addr<num_dft_rt_v6;num_ipv6_addr++)
	    {
            if((ipv6_addr[num_ipv6_addr][0] == data->ipv6_addr[0]) &&
	           (ipv6_addr[num_ipv6_addr][1] == data->ipv6_addr[1]) &&
	           (ipv6_addr[num_ipv6_addr][2] == data->ipv6_addr[2]) &&
	           (ipv6_addr[num_ipv6_addr][3] == data->ipv6_addr[3]))
            {
				return IPACM_FAILURE;
				break;
	        }
	    }

		rt_rule = (struct ipa_ioc_add_rt_rule *)
			 calloc(1, sizeof(struct ipa_ioc_add_rt_rule) +
							NUM_RULES * sizeof(struct ipa_rt_rule_add));

		if (!rt_rule)
		{
			IPACMERR("Error Locate ipa_ioc_add_rt_rule memory...\n");
			return IPACM_FAILURE;
		}

		rt_rule->commit = 1;
		rt_rule->num_rules = NUM_RULES;
		rt_rule->ip = data->iptype;
		strlcpy(rt_rule->rt_tbl_name, IPACM_Iface::ipacmcfg->rt_tbl_v6.name, sizeof(rt_rule->rt_tbl_name));

		rt_rule_entry = &rt_rule->rules[0];
		rt_rule_entry->at_rear = false;
		rt_rule_entry->rule.dst = IPA_CLIENT_APPS_LAN_CONS;  //go to A5
		rt_rule_entry->rule.attrib.attrib_mask = IPA_FLT_DST_ADDR;
		rt_rule_entry->rule.attrib.u.v6.dst_addr[0] = data->ipv6_addr[0];
		rt_rule_entry->rule.attrib.u.v6.dst_addr[1] = data->ipv6_addr[1];
		rt_rule_entry->rule.attrib.u.v6.dst_addr[2] = data->ipv6_addr[2];
		rt_rule_entry->rule.attrib.u.v6.dst_addr[3] = data->ipv6_addr[3];
		rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[0] = 0xFFFFFFFF;
		rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[1] = 0xFFFFFFFF;
		rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[2] = 0xFFFFFFFF;
		rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[3] = 0xFFFFFFFF;
		ipv6_addr[num_dft_rt_v6][0] = data->ipv6_addr[0];
		ipv6_addr[num_dft_rt_v6][1] = data->ipv6_addr[1];
		ipv6_addr[num_dft_rt_v6][2] = data->ipv6_addr[2];
		ipv6_addr[num_dft_rt_v6][3] = data->ipv6_addr[3];
#ifdef FEATURE_IPA_V3
		rt_rule_entry->rule.hashable = true;
#endif
		if (false == m_routing.AddRoutingRule(rt_rule))
		{
			IPACMERR("Routing rule addition failed!\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		else if (rt_rule_entry->status)
		{
			IPACMERR("rt rule adding failed. Result=%d\n", rt_rule_entry->status);
			res = rt_rule_entry->status;
			goto fail;
		}
		dft_rt_rule_hdl[MAX_DEFAULT_v4_ROUTE_RULES + 2*num_dft_rt_v6] = rt_rule_entry->rt_rule_hdl;

		/* setup same rule for v6_wan table*/
		strlcpy(rt_rule->rt_tbl_name, IPACM_Iface::ipacmcfg->rt_tbl_wan_v6.name, sizeof(rt_rule->rt_tbl_name));
		if (false == m_routing.AddRoutingRule(rt_rule))
		{
			IPACMERR("Routing rule addition failed!\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		else if (rt_rule_entry->status)
		{
			IPACMERR("rt rule adding failed. Result=%d\n", rt_rule_entry->status);
			res = rt_rule_entry->status;
			goto fail;
		}
		dft_rt_rule_hdl[MAX_DEFAULT_v4_ROUTE_RULES + 2*num_dft_rt_v6+1] = rt_rule_entry->rt_rule_hdl;

		IPACMDBG_H("ipv6 wan iface rt-rule hdl=0x%x hdl=0x%x, num_dft_rt_v6: %d \n",
		          dft_rt_rule_hdl[MAX_DEFAULT_v4_ROUTE_RULES + 2*num_dft_rt_v6],
		          dft_rt_rule_hdl[MAX_DEFAULT_v4_ROUTE_RULES + 2*num_dft_rt_v6+1],num_dft_rt_v6);

		if (num_dft_rt_v6 == 0)
		{
#ifdef FEATURE_L2TP
			if(ipa_if_cate == WLAN_IF)
			{
				add_tcp_syn_flt_rule(data->iptype);
			}
			else if(ipa_if_cate == ODU_IF)
			{
				add_tcp_syn_flt_rule_l2tp(IPA_IP_v4);
				add_tcp_syn_flt_rule_l2tp(IPA_IP_v6);
			}
#endif
			install_ipv6_icmp_flt_rule();

			/* populate the flt rule offset for eth bridge */
			eth_bridge_flt_rule_offset[data->iptype] = ipv6_icmp_flt_rule_hdl[0];
			eth_bridge_post_event(IPA_ETH_BRIDGE_IFACE_UP, IPA_IP_v6, NULL, NULL, NULL);

			init_fl_rule(data->iptype);
		}
		num_dft_rt_v6++;
		IPACMDBG_H("number of default route rules %d\n", num_dft_rt_v6);
	}

	IPACMDBG_H("finish route/filter rule ip-type: %d, res(%d)\n", data->iptype, res);

fail:
	free(rt_rule);
	return res;
}

/* configure private subnet filter rules*/
int IPACM_Lan::handle_private_subnet(ipa_ip_type iptype)
{
	struct ipa_flt_rule_add flt_rule_entry;
	int i;

	ipa_ioc_add_flt_rule *m_pFilteringTable;

	IPACMDBG_H("lan->handle_private_subnet(); set route/filter rule \n");

	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	if (iptype == IPA_IP_v4)
	{

		m_pFilteringTable = (struct ipa_ioc_add_flt_rule *)
			 calloc(1,
							sizeof(struct ipa_ioc_add_flt_rule) +
							(IPACM_Iface::ipacmcfg->ipa_num_private_subnet) * sizeof(struct ipa_flt_rule_add)
							);
		if (!m_pFilteringTable)
		{
			PERROR("Error Locate ipa_flt_rule_add memory...\n");
			return IPACM_FAILURE;
		}
		m_pFilteringTable->commit = 1;
		m_pFilteringTable->ep = rx_prop->rx[0].src_pipe;
		m_pFilteringTable->global = false;
		m_pFilteringTable->ip = IPA_IP_v4;
		m_pFilteringTable->num_rules = (uint8_t)IPACM_Iface::ipacmcfg->ipa_num_private_subnet;

		/* Make LAN-traffic always go A5, use default IPA-RT table */
		if (false == m_routing.GetRoutingTable(&IPACM_Iface::ipacmcfg->rt_tbl_default_v4))
		{
			IPACMERR("LAN m_routing.GetRoutingTable(&IPACM_Iface::ipacmcfg->rt_tbl_default_v4=0x%p) Failed.\n", &IPACM_Iface::ipacmcfg->rt_tbl_default_v4);
			free(m_pFilteringTable);
			return IPACM_FAILURE;
		}

		for (i = 0; i < (IPACM_Iface::ipacmcfg->ipa_num_private_subnet); i++)
		{
			memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));
			flt_rule_entry.at_rear = true;
			flt_rule_entry.rule.retain_hdr = 1;
			flt_rule_entry.flt_rule_hdl = -1;
			flt_rule_entry.status = -1;
			flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
#ifdef FEATURE_IPA_V3
			flt_rule_entry.rule.hashable = true;
#endif
                        /* Support private subnet feature including guest-AP can't talk to primary AP etc */
			flt_rule_entry.rule.rt_tbl_hdl = IPACM_Iface::ipacmcfg->rt_tbl_default_v4.hdl;
			IPACMDBG_H(" private filter rule use table: %s\n",IPACM_Iface::ipacmcfg->rt_tbl_default_v4.name);

			memcpy(&flt_rule_entry.rule.attrib,
						 &rx_prop->rx[0].attrib,
						 sizeof(flt_rule_entry.rule.attrib));
			flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
			flt_rule_entry.rule.attrib.u.v4.dst_addr_mask = IPACM_Iface::ipacmcfg->private_subnet_table[i].subnet_mask;
			flt_rule_entry.rule.attrib.u.v4.dst_addr = IPACM_Iface::ipacmcfg->private_subnet_table[i].subnet_addr;
			memcpy(&(m_pFilteringTable->rules[i]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));
			IPACMDBG_H("Loop %d  5\n", i);
		}

		if (false == m_filtering.AddFilteringRule(m_pFilteringTable))
		{
			IPACMERR("Error Adding RuleTable(0) to Filtering, aborting...\n");
			free(m_pFilteringTable);
			return IPACM_FAILURE;
		}
		IPACM_Iface::ipacmcfg->increaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4, IPACM_Iface::ipacmcfg->ipa_num_private_subnet);

		/* copy filter rule hdls */
		for (i = 0; i < IPACM_Iface::ipacmcfg->ipa_num_private_subnet; i++)
		{
			private_fl_rule_hdl[i] = m_pFilteringTable->rules[i].flt_rule_hdl;
		}
		free(m_pFilteringTable);
	}
	else
	{
		IPACMDBG_H("No private subnet rules for ipv6 iface %s\n", dev_name);
	}
	return IPACM_SUCCESS;
}


/* for STA mode wan up:  configure filter rule for wan_up event*/
int IPACM_Lan::handle_wan_up(ipa_ip_type ip_type)
{
	struct ipa_flt_rule_add flt_rule_entry;
	int len = 0;
	ipa_ioc_add_flt_rule *m_pFilteringTable;

	IPACMDBG_H("set WAN interface as default filter rule\n");

	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	if(ip_type == IPA_IP_v4)
	{
		len = sizeof(struct ipa_ioc_add_flt_rule) + (1 * sizeof(struct ipa_flt_rule_add));
		m_pFilteringTable = (struct ipa_ioc_add_flt_rule *)calloc(1, len);
		if (m_pFilteringTable == NULL)
		{
			PERROR("Error Locate ipa_flt_rule_add memory...\n");
			return IPACM_FAILURE;
		}

		m_pFilteringTable->commit = 1;
		m_pFilteringTable->ep = rx_prop->rx[0].src_pipe;
		m_pFilteringTable->global = false;
		m_pFilteringTable->ip = IPA_IP_v4;
		m_pFilteringTable->num_rules = (uint8_t)1;

		IPACMDBG_H("Retrieving routing hanle for table: %s\n",
						 IPACM_Iface::ipacmcfg->rt_tbl_wan_v4.name);
		if (false == m_routing.GetRoutingTable(&IPACM_Iface::ipacmcfg->rt_tbl_wan_v4))
		{
			IPACMERR("m_routing.GetRoutingTable(&IPACM_Iface::ipacmcfg->rt_tbl_wan_v4=0x%p) Failed.\n",
							 &IPACM_Iface::ipacmcfg->rt_tbl_wan_v4);
			free(m_pFilteringTable);
			return IPACM_FAILURE;
		}
		IPACMDBG_H("Routing hanle for table: %d\n", IPACM_Iface::ipacmcfg->rt_tbl_wan_v4.hdl);


		memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add)); // Zero All Fields
		flt_rule_entry.at_rear = true;
		flt_rule_entry.flt_rule_hdl = -1;
		flt_rule_entry.status = -1;
		if(IPACM_Wan::isWan_Bridge_Mode())
		{
			flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
		}
		else
		{
			flt_rule_entry.rule.action = IPA_PASS_TO_SRC_NAT; //IPA_PASS_TO_ROUTING
		}
#ifdef FEATURE_IPA_V3
		flt_rule_entry.rule.hashable = true;
#endif
		flt_rule_entry.rule.rt_tbl_hdl = IPACM_Iface::ipacmcfg->rt_tbl_wan_v4.hdl;

		memcpy(&flt_rule_entry.rule.attrib,
					 &rx_prop->rx[0].attrib,
					 sizeof(flt_rule_entry.rule.attrib));

		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
		flt_rule_entry.rule.attrib.u.v4.dst_addr_mask = 0x0;
		flt_rule_entry.rule.attrib.u.v4.dst_addr = 0x0;

		memcpy(&m_pFilteringTable->rules[0], &flt_rule_entry, sizeof(flt_rule_entry));
		if (false == m_filtering.AddFilteringRule(m_pFilteringTable))
		{
			IPACMERR("Error Adding RuleTable(0) to Filtering, aborting...\n");
			free(m_pFilteringTable);
			return IPACM_FAILURE;
		}
		else
		{
			IPACM_Iface::ipacmcfg->increaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4, 1);
			IPACMDBG_H("flt rule hdl0=0x%x, status=0x%x\n",
							 m_pFilteringTable->rules[0].flt_rule_hdl,
							 m_pFilteringTable->rules[0].status);
		}


		/* copy filter hdls  */
		lan_wan_fl_rule_hdl[0] = m_pFilteringTable->rules[0].flt_rule_hdl;
		free(m_pFilteringTable);
	}
	else if(ip_type == IPA_IP_v6)
	{
		/* add default v6 filter rule */
		m_pFilteringTable = (struct ipa_ioc_add_flt_rule *)
			 calloc(1, sizeof(struct ipa_ioc_add_flt_rule) +
					1 * sizeof(struct ipa_flt_rule_add));

		if (!m_pFilteringTable)
		{
			PERROR("Error Locate ipa_flt_rule_add memory...\n");
			return IPACM_FAILURE;
		}

		m_pFilteringTable->commit = 1;
		m_pFilteringTable->ep = rx_prop->rx[0].src_pipe;
		m_pFilteringTable->global = false;
		m_pFilteringTable->ip = IPA_IP_v6;
		m_pFilteringTable->num_rules = (uint8_t)1;

		if (false == m_routing.GetRoutingTable(&IPACM_Iface::ipacmcfg->rt_tbl_v6))
		{
			IPACMERR("m_routing.GetRoutingTable(&IPACM_Iface::ipacmcfg->rt_tbl_v6=0x%p) Failed.\n", &IPACM_Iface::ipacmcfg->rt_tbl_v6);
			free(m_pFilteringTable);
			return IPACM_FAILURE;
		}

		memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));

		flt_rule_entry.at_rear = true;
		flt_rule_entry.flt_rule_hdl = -1;
		flt_rule_entry.status = -1;
		flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
#ifdef FEATURE_IPA_V3
		flt_rule_entry.rule.hashable = true;
#endif
		flt_rule_entry.rule.rt_tbl_hdl = IPACM_Iface::ipacmcfg->rt_tbl_v6.hdl;

		memcpy(&flt_rule_entry.rule.attrib,
					 &rx_prop->rx[0].attrib,
					 sizeof(flt_rule_entry.rule.attrib));

		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
		flt_rule_entry.rule.attrib.u.v6.dst_addr_mask[0] = 0x00000000;
		flt_rule_entry.rule.attrib.u.v6.dst_addr_mask[1] = 0x00000000;
		flt_rule_entry.rule.attrib.u.v6.dst_addr_mask[2] = 0x00000000;
		flt_rule_entry.rule.attrib.u.v6.dst_addr_mask[3] = 0x00000000;
		flt_rule_entry.rule.attrib.u.v6.dst_addr[0] = 0X00000000;
		flt_rule_entry.rule.attrib.u.v6.dst_addr[1] = 0x00000000;
		flt_rule_entry.rule.attrib.u.v6.dst_addr[2] = 0x00000000;
		flt_rule_entry.rule.attrib.u.v6.dst_addr[3] = 0X00000000;

		memcpy(&(m_pFilteringTable->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));
		if (false == m_filtering.AddFilteringRule(m_pFilteringTable))
		{
			IPACMERR("Error Adding Filtering rule, aborting...\n");
			free(m_pFilteringTable);
			return IPACM_FAILURE;
		}
		else
		{
			IPACM_Iface::ipacmcfg->increaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, 1);
			IPACMDBG_H("flt rule hdl0=0x%x, status=0x%x\n", m_pFilteringTable->rules[0].flt_rule_hdl, m_pFilteringTable->rules[0].status);
		}

		/* copy filter hdls */
		dft_v6fl_rule_hdl[IPV6_DEFAULT_FILTERTING_RULES] = m_pFilteringTable->rules[0].flt_rule_hdl;
		free(m_pFilteringTable);
	}

	return IPACM_SUCCESS;
}

int IPACM_Lan::handle_wan_up_ex(ipacm_ext_prop *ext_prop, ipa_ip_type iptype, uint8_t xlat_mux_id)
{
	int fd, ret = IPACM_SUCCESS, cnt;
	IPACM_Config* ipacm_config = IPACM_Iface::ipacmcfg;
	struct ipa_ioc_write_qmapid mux;
	int i=0;

	if(rx_prop != NULL)
	{
		/* give mud ID to IPA-driver for WLAN/LAN pkts */
		fd = open(IPA_DEVICE_NAME, O_RDWR);
		if (0 == fd)
		{
			IPACMDBG_H("Failed opening %s.\n", IPA_DEVICE_NAME);
			return IPACM_FAILURE;
		}

		mux.qmap_id = ipacm_config->GetQmapId();
		for(cnt=0; cnt<rx_prop->num_rx_props; cnt++)
		{
			mux.client = rx_prop->rx[cnt].src_pipe;
			ret = ioctl(fd, IPA_IOC_WRITE_QMAPID, &mux);
			if (ret)
			{
				IPACMERR("Failed to write mux id %d\n", mux.qmap_id);
				close(fd);
				return IPACM_FAILURE;
			}
		}
		close(fd);
	}

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	/* Install filter rules for the client. */
	if (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable == true)
	{
		if (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable_wan_set == false)
		{
			if (enable_per_client_stats(&IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable))
			{
				IPACMERR("Failed to enable per client stats %d\n", IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable);
				return IPACM_FAILURE;
			}
			IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable_wan_set = true;
		}
	}
#endif

	/* check only add static UL filter rule once */
	if (num_dft_rt_v6 ==1 && iptype ==IPA_IP_v6 && modem_ul_v6_set == FALSE)
	{
		IPACMDBG_H("IPA_IP_v6 num_dft_rt_v6 %d xlat_mux_id: %d modem_ul_v6_set: %d\n", num_dft_rt_v6, xlat_mux_id, modem_ul_v6_set);
		ret = handle_uplink_filter_rule(ext_prop, iptype, xlat_mux_id);
		modem_ul_v6_set = true;
	} else if (iptype ==IPA_IP_v4 && modem_ul_v4_set == false) {
		IPACMDBG_H("IPA_IP_v4 xlat_mux_id: %d, modem_ul_v4_set %d\n", xlat_mux_id, modem_ul_v4_set);
		ret = handle_uplink_filter_rule(ext_prop, iptype, xlat_mux_id);
		modem_ul_v4_set = true;
	} else {
			IPACMDBG_H("ip-type: %d modem_ul_v4_set: %d, modem_ul_v6_set %d\n",
				iptype, modem_ul_v4_set, modem_ul_v6_set);

	}

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	/* Install filter rules for the client. */
	if (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable == true)
	{
		IPACMDBG_H("xlat_mux_id: %d, iptype %d\n", xlat_mux_id, iptype);
		ret = install_uplink_filter_rule(ext_prop, iptype, xlat_mux_id);
	}
#endif

	return ret;
}

/* handle ETH client initial, construct full headers (tx property) */
int IPACM_Lan::handle_eth_hdr_init(uint8_t *mac_addr)
{

#define ETH_IFACE_INDEX_LEN 2

	int res = IPACM_SUCCESS, len = 0;
	char index[ETH_IFACE_INDEX_LEN];
	struct ipa_ioc_copy_hdr sCopyHeader;
	struct ipa_ioc_add_hdr *pHeaderDescriptor = NULL;
	uint32_t cnt;
	int clnt_indx;
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	struct wan_ioctl_lan_client_info *client_info;
	ipacm_ext_prop* ext_prop;
	int max_clients = (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable) ? IPA_MAX_NUM_HW_PATH_CLIENTS:
		IPA_MAX_NUM_ETH_CLIENTS;
#else
	int max_clients = IPA_MAX_NUM_ETH_CLIENTS;
#endif

	clnt_indx = get_eth_client_index(mac_addr);

	if (clnt_indx != IPACM_INVALID_INDEX)
	{
		IPACMERR("eth client is found/attached already with index %d \n", clnt_indx);
		return IPACM_FAILURE;
	}

	/* add header to IPA */
	if (num_eth_client >= max_clients)
	{
		IPACMERR("Reached maximum number(%d) of eth clients\n", max_clients);
		return IPACM_FAILURE;
	}

	IPACMDBG_H("ETH client number: %d\n", num_eth_client);

	memcpy(get_client_memptr(eth_client, num_eth_client)->mac,
				 mac_addr,
				 sizeof(get_client_memptr(eth_client, num_eth_client)->mac));


	IPACMDBG_H("Received Client MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					 mac_addr[0], mac_addr[1], mac_addr[2],
					 mac_addr[3], mac_addr[4], mac_addr[5]);

	IPACMDBG_H("stored MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					 get_client_memptr(eth_client, num_eth_client)->mac[0],
					 get_client_memptr(eth_client, num_eth_client)->mac[1],
					 get_client_memptr(eth_client, num_eth_client)->mac[2],
					 get_client_memptr(eth_client, num_eth_client)->mac[3],
					 get_client_memptr(eth_client, num_eth_client)->mac[4],
					 get_client_memptr(eth_client, num_eth_client)->mac[5]);

	/* add header to IPA */
	if(tx_prop != NULL)
	{
		len = sizeof(struct ipa_ioc_add_hdr) + (1 * sizeof(struct ipa_hdr_add));
		pHeaderDescriptor = (struct ipa_ioc_add_hdr *)calloc(1, len);
		if (pHeaderDescriptor == NULL)
		{
			IPACMERR("calloc failed to allocate pHeaderDescriptor\n");
			return IPACM_FAILURE;
		}

		/* copy partial header for v4*/
		for (cnt=0; cnt<tx_prop->num_tx_props; cnt++)
		{
				 if(tx_prop->tx[cnt].ip==IPA_IP_v4)
				 {
								IPACMDBG_H("Got partial v4-header name from %d tx props\n", cnt);
								memset(&sCopyHeader, 0, sizeof(sCopyHeader));
								memcpy(sCopyHeader.name,
											 tx_prop->tx[cnt].hdr_name,
											 sizeof(sCopyHeader.name));

								IPACMDBG_H("header name: %s in tx:%d\n", sCopyHeader.name,cnt);
								if (m_header.CopyHeader(&sCopyHeader) == false)
								{
									PERROR("ioctl copy header failed");
									res = IPACM_FAILURE;
									goto fail;
								}

								IPACMDBG_H("header length: %d, paritial: %d\n", sCopyHeader.hdr_len, sCopyHeader.is_partial);
								IPACMDBG_H("header eth2_ofst_valid: %d, eth2_ofst: %d\n", sCopyHeader.is_eth2_ofst_valid, sCopyHeader.eth2_ofst);
								if (sCopyHeader.hdr_len > IPA_HDR_MAX_SIZE)
								{
									IPACMERR("header oversize\n");
									res = IPACM_FAILURE;
									goto fail;
								}
								else
								{
									memcpy(pHeaderDescriptor->hdr[0].hdr,
												 sCopyHeader.hdr,
												 sCopyHeader.hdr_len);
								}

								/* copy client mac_addr to partial header */
								if (sCopyHeader.is_eth2_ofst_valid)
								{
									memcpy(&pHeaderDescriptor->hdr[0].hdr[sCopyHeader.eth2_ofst],
											 mac_addr,
											 IPA_MAC_ADDR_SIZE);
								}
								/* replace src mac to bridge mac_addr if any  */
								if (IPACM_Iface::ipacmcfg->ipa_bridge_enable)
								{
									memcpy(&pHeaderDescriptor->hdr[0].hdr[sCopyHeader.eth2_ofst+IPA_MAC_ADDR_SIZE],
											IPACM_Iface::ipacmcfg->bridge_mac,
											IPA_MAC_ADDR_SIZE);
									IPACMDBG_H("device is in bridge mode \n");
								}

								pHeaderDescriptor->commit = true;
								pHeaderDescriptor->num_hdrs = 1;

								memset(pHeaderDescriptor->hdr[0].name, 0,
											 sizeof(pHeaderDescriptor->hdr[0].name));

								snprintf(index,sizeof(index), "%d", ipa_if_num);
								strlcpy(pHeaderDescriptor->hdr[0].name, index, sizeof(pHeaderDescriptor->hdr[0].name));
								pHeaderDescriptor->hdr[0].name[IPA_RESOURCE_NAME_MAX-1] = '\0';
								if (strlcat(pHeaderDescriptor->hdr[0].name, IPA_ETH_HDR_NAME_v4, sizeof(pHeaderDescriptor->hdr[0].name)) > IPA_RESOURCE_NAME_MAX)
								{
									IPACMERR(" header name construction failed exceed length (%d)\n", strlen(pHeaderDescriptor->hdr[0].name));
									res = IPACM_FAILURE;
									goto fail;
								}

								snprintf(index,sizeof(index), "%d", header_name_count);
								if (strlcat(pHeaderDescriptor->hdr[0].name, index, sizeof(pHeaderDescriptor->hdr[0].name)) > IPA_RESOURCE_NAME_MAX)
								{
									IPACMERR(" header name construction failed exceed length (%d)\n", strlen(pHeaderDescriptor->hdr[0].name));
									res = IPACM_FAILURE;
									goto fail;
								}

								pHeaderDescriptor->hdr[0].hdr_len = sCopyHeader.hdr_len;
								hdr_len = sCopyHeader.hdr_len;
								pHeaderDescriptor->hdr[0].hdr_hdl = -1;
								pHeaderDescriptor->hdr[0].is_partial = 0;
								pHeaderDescriptor->hdr[0].status = -1;

					 if (m_header.AddHeader(pHeaderDescriptor) == false ||
							pHeaderDescriptor->hdr[0].status != 0)
					 {
						IPACMERR("ioctl IPA_IOC_ADD_HDR failed: %d\n", pHeaderDescriptor->hdr[0].status);
						res = IPACM_FAILURE;
						goto fail;
					 }

					get_client_memptr(eth_client, num_eth_client)->hdr_hdl_v4 = pHeaderDescriptor->hdr[0].hdr_hdl;
					IPACMDBG_H("eth-client(%d) v4 full header name:%s header handle:(0x%x), Len:%d\n",
												 num_eth_client,
												 pHeaderDescriptor->hdr[0].name,
												 get_client_memptr(eth_client, num_eth_client)->hdr_hdl_v4,
												 hdr_len);
					get_client_memptr(eth_client, num_eth_client)->ipv4_header_set=true;

					break;
				 }
		}


		/* copy partial header for v6*/
		for (cnt=0; cnt<tx_prop->num_tx_props; cnt++)
		{
			if(tx_prop->tx[cnt].ip==IPA_IP_v6)
			{

				IPACMDBG_H("Got partial v6-header name from %d tx props\n", cnt);
				memset(&sCopyHeader, 0, sizeof(sCopyHeader));
				memcpy(sCopyHeader.name,
						tx_prop->tx[cnt].hdr_name,
							sizeof(sCopyHeader.name));

				IPACMDBG_H("header name: %s in tx:%d\n", sCopyHeader.name,cnt);
				if (m_header.CopyHeader(&sCopyHeader) == false)
				{
					PERROR("ioctl copy header failed");
					res = IPACM_FAILURE;
					goto fail;
				}

				IPACMDBG_H("header length: %d, paritial: %d\n", sCopyHeader.hdr_len, sCopyHeader.is_partial);
				IPACMDBG_H("header eth2_ofst_valid: %d, eth2_ofst: %d\n", sCopyHeader.is_eth2_ofst_valid, sCopyHeader.eth2_ofst);
				if (sCopyHeader.hdr_len > IPA_HDR_MAX_SIZE)
				{
					IPACMERR("header oversize\n");
					res = IPACM_FAILURE;
					goto fail;
				}
				else
				{
					memcpy(pHeaderDescriptor->hdr[0].hdr,
							sCopyHeader.hdr,
								sCopyHeader.hdr_len);
				}

				/* copy client mac_addr to partial header */
				if (sCopyHeader.is_eth2_ofst_valid)
				{
					memcpy(&pHeaderDescriptor->hdr[0].hdr[sCopyHeader.eth2_ofst],
						mac_addr,
						IPA_MAC_ADDR_SIZE);
				}
				/* replace src mac to bridge mac_addr if any  */
				if (IPACM_Iface::ipacmcfg->ipa_bridge_enable)
				{
					memcpy(&pHeaderDescriptor->hdr[0].hdr[sCopyHeader.eth2_ofst+IPA_MAC_ADDR_SIZE],
							IPACM_Iface::ipacmcfg->bridge_mac,
							IPA_MAC_ADDR_SIZE);
					IPACMDBG_H("device is in bridge mode \n");
				}

				pHeaderDescriptor->commit = true;
				pHeaderDescriptor->num_hdrs = 1;

				memset(pHeaderDescriptor->hdr[0].name, 0,
					 sizeof(pHeaderDescriptor->hdr[0].name));

				snprintf(index,sizeof(index), "%d", ipa_if_num);
				strlcpy(pHeaderDescriptor->hdr[0].name, index, sizeof(pHeaderDescriptor->hdr[0].name));
				pHeaderDescriptor->hdr[0].name[IPA_RESOURCE_NAME_MAX-1] = '\0';
				if (strlcat(pHeaderDescriptor->hdr[0].name, IPA_ETH_HDR_NAME_v6, sizeof(pHeaderDescriptor->hdr[0].name)) > IPA_RESOURCE_NAME_MAX)
				{
					IPACMERR(" header name construction failed exceed length (%d)\n", strlen(pHeaderDescriptor->hdr[0].name));
					res = IPACM_FAILURE;
					goto fail;
				}
				snprintf(index,sizeof(index), "%d", header_name_count);
				if (strlcat(pHeaderDescriptor->hdr[0].name, index, sizeof(pHeaderDescriptor->hdr[0].name)) > IPA_RESOURCE_NAME_MAX)
				{
					IPACMERR(" header name construction failed exceed length (%d)\n", strlen(pHeaderDescriptor->hdr[0].name));
					res = IPACM_FAILURE;
					goto fail;
				}

				pHeaderDescriptor->hdr[0].hdr_len = sCopyHeader.hdr_len;
				hdr_len = sCopyHeader.hdr_len;
				pHeaderDescriptor->hdr[0].hdr_hdl = -1;
				pHeaderDescriptor->hdr[0].is_partial = 0;
				pHeaderDescriptor->hdr[0].status = -1;

				if (m_header.AddHeader(pHeaderDescriptor) == false ||
						pHeaderDescriptor->hdr[0].status != 0)
				{
					IPACMERR("ioctl IPA_IOC_ADD_HDR failed: %d\n", pHeaderDescriptor->hdr[0].status);
					res = IPACM_FAILURE;
					goto fail;
				}

				get_client_memptr(eth_client, num_eth_client)->hdr_hdl_v6 = pHeaderDescriptor->hdr[0].hdr_hdl;
				IPACMDBG_H("eth-client(%d) v6 full header name:%s header handle:(0x%x) Len:%d\n",
						 num_eth_client,
						 pHeaderDescriptor->hdr[0].name,
									 get_client_memptr(eth_client, num_eth_client)->hdr_hdl_v6,
									 hdr_len);

									get_client_memptr(eth_client, num_eth_client)->ipv6_header_set=true;

				break;

			}
		}
		/* initialize wifi client*/
		get_client_memptr(eth_client, num_eth_client)->route_rule_set_v4 = false;
		get_client_memptr(eth_client, num_eth_client)->route_rule_set_v6 = 0;
		get_client_memptr(eth_client, num_eth_client)->ipv4_set = false;
		get_client_memptr(eth_client, num_eth_client)->ipv6_set = 0;
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
		get_client_memptr(eth_client, num_eth_client)->ipv4_ul_rules_set = false;
		get_client_memptr(eth_client, num_eth_client)->ipv4_ul_rules_set = false;
		get_client_memptr(eth_client, num_eth_client)->lan_stats_idx = get_lan_stats_index(get_client_memptr(eth_client, num_eth_client)->mac);
		memset(get_client_memptr(eth_client, num_eth_client)->wan_ul_fl_rule_hdl_v4, 0, MAX_WAN_UL_FILTER_RULES * sizeof(uint32_t));
		memset(get_client_memptr(eth_client, num_eth_client)->wan_ul_fl_rule_hdl_v6, 0, MAX_WAN_UL_FILTER_RULES * sizeof(uint32_t));
#endif
		clnt_indx = num_eth_client;
		num_eth_client++;
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
		if (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable == true &&
			get_client_memptr(eth_client, num_eth_client)->lan_stats_idx != -1)
		{
			/* Store the client info at WAN driver. */
			client_info = (struct wan_ioctl_lan_client_info *)malloc(sizeof(struct wan_ioctl_lan_client_info));
			if (client_info == NULL)
			{
				IPACMERR("Unable to allocate memory\n");
				res = IPACM_FAILURE;
				goto fail;
			}
			memset(client_info, 0, sizeof(struct wan_ioctl_lan_client_info));
			if (ipa_if_cate == LAN_IF)
			{
				client_info->device_type = IPACM_CLIENT_DEVICE_TYPE_USB;
			}
			else if (ipa_if_cate == ODU_IF)
			{
				client_info->device_type = IPACM_CLIENT_DEVICE_TYPE_ETH;
			}
			else
			{
				IPACMERR("Unsupported interface category: %d\n", ipa_if_cate);
				res = IPACM_FAILURE;
				goto fail;
			}
			memcpy(client_info->mac,
					mac_addr,
					IPA_MAC_ADDR_SIZE);
			client_info->client_init = 1;
			client_info->client_idx = get_client_memptr(eth_client, clnt_indx)->lan_stats_idx;
			client_info->ul_src_pipe = IPA_CLIENT_MAX;
			client_info->hdr_len = hdr_len;
			if (rx_prop)
			{
				client_info->ul_src_pipe = rx_prop->rx[0].src_pipe;
			}
			if (set_lan_client_info(client_info))
			{
				res = IPACM_FAILURE;
				free(client_info);
				/* Reset the mac from active list. */
				reset_active_lan_stats_index(get_client_memptr(eth_client, clnt_indx)->lan_stats_idx, mac_addr);
				/* Add the mac to inactive list. */
				get_free_inactive_lan_stats_index(mac_addr);
				get_client_memptr(eth_client, clnt_indx)->lan_stats_idx = -1;
				goto fail;
			}
			free(client_info);
			if (IPACM_Wan::isWanUP(ipa_if_num))
			{
				if(IPACM_Wan::backhaul_is_sta_mode == false)
				{
					ext_prop = IPACM_Iface::ipacmcfg->GetExtProp(IPA_IP_v4);
					install_uplink_filter_rule_per_client(ext_prop, IPA_IP_v4, IPACM_Wan::getXlat_Mux_Id(),
						get_client_memptr(eth_client, clnt_indx)->mac);
					get_client_memptr(eth_client, clnt_indx)->ipv4_ul_rules_set = true;
				}
			}
			if(IPACM_Wan::isWanUP_V6(ipa_if_num))
			{
				if(IPACM_Wan::backhaul_is_sta_mode == false)
				{
					ext_prop = IPACM_Iface::ipacmcfg->GetExtProp(IPA_IP_v6);
					install_uplink_filter_rule_per_client(ext_prop, IPA_IP_v6, 0,
					 get_client_memptr(eth_client, clnt_indx)->mac);
					get_client_memptr(eth_client, clnt_indx)->ipv6_ul_rules_set = true;
				}
			}
		}
#endif
		header_name_count++; //keep increasing header_name_count
		res = IPACM_SUCCESS;
		IPACMDBG_H("eth client number: %d\n", num_eth_client);
	}
	else
	{
		return res;
	}
fail:
	free(pHeaderDescriptor);
	return res;
}

/*handle eth client */
int IPACM_Lan::handle_eth_client_ipaddr(ipacm_event_data_all *data)
{
	int clnt_indx;
	int v6_num;
	uint32_t ipv6_link_local_prefix = 0xFE800000;
	uint32_t ipv6_link_local_prefix_mask = 0xFFC00000;

	IPACMDBG_H("number of eth clients: %d\n", num_eth_client);
	IPACMDBG_H("event MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					 data->mac_addr[0],
					 data->mac_addr[1],
					 data->mac_addr[2],
					 data->mac_addr[3],
					 data->mac_addr[4],
					 data->mac_addr[5]);

	clnt_indx = get_eth_client_index(data->mac_addr);

		if (clnt_indx == IPACM_INVALID_INDEX)
		{
			IPACMERR("eth client not found/attached \n");
			return IPACM_FAILURE;
		}

	IPACMDBG_H("Ip-type received %d\n", data->iptype);
	if (data->iptype == IPA_IP_v4)
	{
		IPACMDBG_H("ipv4 address: 0x%x\n", data->ipv4_addr);
		if (data->ipv4_addr != 0) /* not 0.0.0.0 */
		{
			if (get_client_memptr(eth_client, clnt_indx)->ipv4_set == false)
			{
				get_client_memptr(eth_client, clnt_indx)->v4_addr = data->ipv4_addr;
				get_client_memptr(eth_client, clnt_indx)->ipv4_set = true;
			}
			else
			{
			   /* check if client got new IPv4 address*/
			   if(data->ipv4_addr == get_client_memptr(eth_client, clnt_indx)->v4_addr)
			   {
				IPACMDBG_H("Already setup ipv4 addr for client:%d, ipv4 address didn't change\n", clnt_indx);
				 return IPACM_FAILURE;
			   }
			   else
			   {
					IPACMDBG_H("ipv4 addr for client:%d is changed \n", clnt_indx);
					/* delete NAT rules first */
					CtList->HandleNeighIpAddrDelEvt(get_client_memptr(eth_client, clnt_indx)->v4_addr);
					delete_eth_rtrules(clnt_indx,IPA_IP_v4);
					get_client_memptr(eth_client, clnt_indx)->route_rule_set_v4 = false;
					get_client_memptr(eth_client, clnt_indx)->v4_addr = data->ipv4_addr;
				}
			}
		}
		else
		{
		    IPACMDBG_H("Invalid client IPv4 address \n");
		    return IPACM_FAILURE;
		}
	}
	else
	{
		if ((data->ipv6_addr[0] != 0) || (data->ipv6_addr[1] != 0) ||
				(data->ipv6_addr[2] != 0) || (data->ipv6_addr[3] || 0)) /* check if all 0 not valid ipv6 address */
		{
			IPACMDBG_H("ipv6 address: 0x%x:%x:%x:%x\n", data->ipv6_addr[0], data->ipv6_addr[1], data->ipv6_addr[2], data->ipv6_addr[3]);
			if( (data->ipv6_addr[0] & ipv6_link_local_prefix_mask) != (ipv6_link_local_prefix & ipv6_link_local_prefix_mask) &&
				memcmp(ipv6_prefix, data->ipv6_addr, sizeof(ipv6_prefix)) != 0)
			{
				IPACMDBG_H("This IPv6 address is not global IPv6 address with correct prefix, ignore.\n");
				return IPACM_FAILURE;
			}

            if(get_client_memptr(eth_client, clnt_indx)->ipv6_set < IPV6_NUM_ADDR)
			{

		       for(v6_num=0;v6_num < get_client_memptr(eth_client, clnt_indx)->ipv6_set;v6_num++)
				{
					if( data->ipv6_addr[0] == get_client_memptr(eth_client, clnt_indx)->v6_addr[v6_num][0] &&
			           data->ipv6_addr[1] == get_client_memptr(eth_client, clnt_indx)->v6_addr[v6_num][1] &&
			  	        data->ipv6_addr[2]== get_client_memptr(eth_client, clnt_indx)->v6_addr[v6_num][2] &&
			  	         data->ipv6_addr[3] == get_client_memptr(eth_client, clnt_indx)->v6_addr[v6_num][3])
					{
						IPACMDBG_H("Already see this ipv6 addr at position: %d for client:%d\n", v6_num, clnt_indx);
						return IPACM_FAILURE; /* not setup the RT rules*/
					}
				}

		       /* not see this ipv6 before for wifi client*/
			   get_client_memptr(eth_client, clnt_indx)->v6_addr[get_client_memptr(eth_client, clnt_indx)->ipv6_set][0] = data->ipv6_addr[0];
			   get_client_memptr(eth_client, clnt_indx)->v6_addr[get_client_memptr(eth_client, clnt_indx)->ipv6_set][1] = data->ipv6_addr[1];
			   get_client_memptr(eth_client, clnt_indx)->v6_addr[get_client_memptr(eth_client, clnt_indx)->ipv6_set][2] = data->ipv6_addr[2];
			   get_client_memptr(eth_client, clnt_indx)->v6_addr[get_client_memptr(eth_client, clnt_indx)->ipv6_set][3] = data->ipv6_addr[3];
			   get_client_memptr(eth_client, clnt_indx)->ipv6_set++;
		    }
		    else
		    {
		        IPACMDBG_H("Already got %d ipv6 addr for client:%d\n", IPV6_NUM_ADDR, clnt_indx);
				return IPACM_FAILURE; /* not setup the RT rules*/
		    }
		}
	}

	return IPACM_SUCCESS;
}

/*handle eth client routing rule*/
int IPACM_Lan::handle_eth_client_route_rule(uint8_t *mac_addr, ipa_ip_type iptype)
{
	struct ipa_ioc_add_rt_rule *rt_rule;
	struct ipa_rt_rule_add *rt_rule_entry;
	uint32_t tx_index;
	int eth_index,v6_num;
	const int NUM = 1;

	if(tx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	IPACMDBG_H("Received mac_addr MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					 mac_addr[0], mac_addr[1], mac_addr[2],
					 mac_addr[3], mac_addr[4], mac_addr[5]);

	eth_index = get_eth_client_index(mac_addr);
	if (eth_index == IPACM_INVALID_INDEX)
	{
		IPACMDBG_H("eth client not found/attached \n");
		return IPACM_SUCCESS;
	}

	if (iptype==IPA_IP_v4) {
		IPACMDBG_H("eth client index: %d, ip-type: %d, ipv4_set:%d, ipv4_rule_set:%d \n", eth_index, iptype,
					 get_client_memptr(eth_client, eth_index)->ipv4_set,
					 get_client_memptr(eth_client, eth_index)->route_rule_set_v4);
	} else {
		IPACMDBG_H("eth client index: %d, ip-type: %d, ipv6_set:%d, ipv6_rule_num:%d \n", eth_index, iptype,
					 get_client_memptr(eth_client, eth_index)->ipv6_set,
					 get_client_memptr(eth_client, eth_index)->route_rule_set_v6);
	}
	/* Add default routing rules if not set yet */
	if ((iptype == IPA_IP_v4
			 && get_client_memptr(eth_client, eth_index)->route_rule_set_v4 == false
			 && get_client_memptr(eth_client, eth_index)->ipv4_set == true)
			|| (iptype == IPA_IP_v6
		            && get_client_memptr(eth_client, eth_index)->route_rule_set_v6 < get_client_memptr(eth_client, eth_index)->ipv6_set
					))
	{

        /* Add corresponding ipa_rm_resource_name of TX-endpoint up before IPV6 RT-rule set */
		IPACMDBG_H("dev %s add producer dependency\n", dev_name);
		if (tx_prop != NULL)
		{
			IPACMDBG_H("depend Got pipe %d rm index : %d \n", tx_prop->tx[0].dst_pipe, IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[tx_prop->tx[0].dst_pipe]);
			IPACM_Iface::ipacmcfg->AddRmDepend(IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[tx_prop->tx[0].dst_pipe],false);
		}
		rt_rule = (struct ipa_ioc_add_rt_rule *)
			 calloc(1, sizeof(struct ipa_ioc_add_rt_rule) +
						NUM * sizeof(struct ipa_rt_rule_add));

		if (rt_rule == NULL)
		{
			PERROR("Error Locate ipa_ioc_add_rt_rule memory...\n");
			return IPACM_FAILURE;
		}

		rt_rule->commit = 1;
		rt_rule->num_rules = (uint8_t)NUM;
		rt_rule->ip = iptype;

		for (tx_index = 0; tx_index < iface_query->num_tx_props; tx_index++)
		{
			if(iptype != tx_prop->tx[tx_index].ip)
		    {
				IPACMDBG_H("Tx:%d, ip-type: %d conflict ip-type: %d no RT-rule added\n",
						tx_index, tx_prop->tx[tx_index].ip,iptype);
		   	        continue;
		    }

  	   	    rt_rule_entry = &rt_rule->rules[0];
			rt_rule_entry->at_rear = 0;

			if (iptype == IPA_IP_v4)
			{
		        IPACMDBG_H("client index(%d):ipv4 address: 0x%x\n", eth_index,
		  		        get_client_memptr(eth_client, eth_index)->v4_addr);

                IPACMDBG_H("client(%d): v4 header handle:(0x%x)\n",
		  				 eth_index,
		  				 get_client_memptr(eth_client, eth_index)->hdr_hdl_v4);
				strlcpy(rt_rule->rt_tbl_name,
								IPACM_Iface::ipacmcfg->rt_tbl_lan_v4.name,
								sizeof(rt_rule->rt_tbl_name));
				rt_rule->rt_tbl_name[IPA_RESOURCE_NAME_MAX-1] = '\0';
			    rt_rule_entry->rule.dst = tx_prop->tx[tx_index].dst_pipe;
			    memcpy(&rt_rule_entry->rule.attrib,
						 &tx_prop->tx[tx_index].attrib,
						 sizeof(rt_rule_entry->rule.attrib));
			    rt_rule_entry->rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
		   	    rt_rule_entry->rule.hdr_hdl = get_client_memptr(eth_client, eth_index)->hdr_hdl_v4;
				rt_rule_entry->rule.attrib.u.v4.dst_addr = get_client_memptr(eth_client, eth_index)->v4_addr;
				rt_rule_entry->rule.attrib.u.v4.dst_addr_mask = 0xFFFFFFFF;
#ifdef FEATURE_IPA_V3
				rt_rule_entry->rule.hashable = false;
#endif
			    if (false == m_routing.AddRoutingRule(rt_rule))
  	            {
  	          	            IPACMERR("Routing rule addition failed!\n");
  	          	            free(rt_rule);
  	          	            return IPACM_FAILURE;
			    }

			    /* copy ipv4 RT hdl */
		        get_client_memptr(eth_client, eth_index)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v4 =
				rt_rule->rules[0].rt_rule_hdl;
				IPACMDBG_H("tx:%d, rt rule hdl=%x ip-type: %d\n", tx_index,
					get_client_memptr(eth_client, eth_index)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v4, iptype);
  	   	    } else {

		        for(v6_num = get_client_memptr(eth_client, eth_index)->route_rule_set_v6;v6_num < get_client_memptr(eth_client, eth_index)->ipv6_set;v6_num++)
			    {
                    IPACMDBG_H("client(%d): v6 header handle:(0x%x)\n",
		  	    			 eth_index,
		  	    			 get_client_memptr(eth_client, eth_index)->hdr_hdl_v6);

		            /* v6 LAN_RT_TBL */
				strlcpy(rt_rule->rt_tbl_name,
			    					IPACM_Iface::ipacmcfg->rt_tbl_v6.name,
			    					sizeof(rt_rule->rt_tbl_name));
				rt_rule->rt_tbl_name[IPA_RESOURCE_NAME_MAX-1] = '\0';
		            /* Support QCMAP LAN traffic feature, send to A5 */
					rt_rule_entry->rule.dst = IPA_CLIENT_APPS_LAN_CONS;
			        memset(&rt_rule_entry->rule.attrib, 0, sizeof(rt_rule_entry->rule.attrib));
		   	        rt_rule_entry->rule.hdr_hdl = 0;
			        rt_rule_entry->rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[0] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][0];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[1] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][1];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[2] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][2];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[3] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][3];
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[0] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[1] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[2] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[3] = 0xFFFFFFFF;
#ifdef FEATURE_IPA_V3
					rt_rule_entry->rule.hashable = true;
#endif
   	                if (false == m_routing.AddRoutingRule(rt_rule))
  	                {
  	                	    IPACMERR("Routing rule addition failed!\n");
  	                	    free(rt_rule);
  	                	    return IPACM_FAILURE;
			        }

		            get_client_memptr(eth_client, eth_index)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6[v6_num] = rt_rule->rules[0].rt_rule_hdl;
					IPACMDBG_H("tx:%d, rt rule hdl=%x ip-type: %d\n", tx_index,
						get_client_memptr(eth_client, eth_index)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6[v6_num], iptype);

			        /*Copy same rule to v6 WAN RT TBL*/
				strlcpy(rt_rule->rt_tbl_name, IPACM_Iface::ipacmcfg->rt_tbl_wan_v6.name, sizeof(rt_rule->rt_tbl_name));
				rt_rule->rt_tbl_name[IPA_RESOURCE_NAME_MAX-1] = '\0';
				/* Downlink traffic from Wan iface, directly through IPA */
					rt_rule_entry->rule.dst = tx_prop->tx[tx_index].dst_pipe;
			        memcpy(&rt_rule_entry->rule.attrib,
						 &tx_prop->tx[tx_index].attrib,
						 sizeof(rt_rule_entry->rule.attrib));
		   	        rt_rule_entry->rule.hdr_hdl = get_client_memptr(eth_client, eth_index)->hdr_hdl_v6;
			        rt_rule_entry->rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[0] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][0];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[1] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][1];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[2] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][2];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[3] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][3];
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[0] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[1] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[2] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[3] = 0xFFFFFFFF;
#ifdef FEATURE_IPA_V3
					rt_rule_entry->rule.hashable = true;
#endif
		            if (false == m_routing.AddRoutingRule(rt_rule))
		            {
							IPACMERR("Routing rule addition failed!\n");
							free(rt_rule);
							return IPACM_FAILURE;
		            }

		            get_client_memptr(eth_client, eth_index)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6_wan[v6_num] = rt_rule->rules[0].rt_rule_hdl;
					IPACMDBG_H("tx:%d, rt rule hdl=%x ip-type: %d\n", tx_index,
						 get_client_memptr(eth_client, eth_index)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6_wan[v6_num], iptype);
			    }
			}

  	    } /* end of for loop */

		free(rt_rule);

		if (iptype == IPA_IP_v4)
		{
			get_client_memptr(eth_client, eth_index)->route_rule_set_v4 = true;
		}
		else
		{
			get_client_memptr(eth_client, eth_index)->route_rule_set_v6 = get_client_memptr(eth_client, eth_index)->ipv6_set;
		}
	}
	return IPACM_SUCCESS;
}

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
int IPACM_Lan::handle_lan_client_connect(uint8_t *mac_addr)
{
	int eth_index, res = IPACM_SUCCESS;
	ipacm_ext_prop* ext_prop;
	struct wan_ioctl_lan_client_info *client_info;

	IPACMDBG_H("Received mac_addr MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			mac_addr[0], mac_addr[1], mac_addr[2],
			mac_addr[3], mac_addr[4], mac_addr[5]);

	eth_index = get_eth_client_index(mac_addr);
	if (eth_index == IPACM_INVALID_INDEX)
	{
		IPACMDBG_H("wlan client not found/attached \n");
		return IPACM_SUCCESS;
	}

	if (get_client_memptr(eth_client, eth_index)->lan_stats_idx != -1)
	{
		IPACMDBG_H("wlan client already has lan_stats index. \n");
		return IPACM_FAILURE;
	}

	get_client_memptr(eth_client, eth_index)->lan_stats_idx = get_lan_stats_index(mac_addr);

	if (get_client_memptr(eth_client, eth_index)->lan_stats_idx == -1)
	{
		IPACMDBG_H("No active index..abort \n");
		return IPACM_FAILURE;
	}

	if (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable == true)
	{
		client_info = (struct wan_ioctl_lan_client_info *)malloc(sizeof(struct wan_ioctl_lan_client_info));
		if (client_info == NULL)
		{
			IPACMERR("Unable to allocate memory\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		memset(client_info, 0, sizeof(struct wan_ioctl_lan_client_info));
		if (ipa_if_cate == LAN_IF)
		{
			client_info->device_type = IPACM_CLIENT_DEVICE_TYPE_USB;
		}
		else if (ipa_if_cate == ODU_IF)
		{
			client_info->device_type = IPACM_CLIENT_DEVICE_TYPE_ETH;
		}
		else
		{
			IPACMERR("Unsupported interface category: %d\n", ipa_if_cate);
			res = IPACM_FAILURE;
			goto fail;
		}
		memcpy(client_info->mac,
				get_client_memptr(eth_client, eth_index)->mac,
				IPA_MAC_ADDR_SIZE);
		client_info->client_init = 1;
		client_info->client_idx = get_client_memptr(eth_client, eth_index)->lan_stats_idx;
		client_info->ul_src_pipe = IPA_CLIENT_MAX;
		client_info->hdr_len = hdr_len;
		if (rx_prop)
		{
			client_info->ul_src_pipe = rx_prop->rx[0].src_pipe;
		}
		if (set_lan_client_info(client_info))
		{
			res = IPACM_FAILURE;
			free(client_info);
			goto fail;
		}
		free(client_info);
		if (IPACM_Wan::isWanUP(ipa_if_num))
		{
			if(IPACM_Wan::backhaul_is_sta_mode == false)
			{
				ext_prop = IPACM_Iface::ipacmcfg->GetExtProp(IPA_IP_v4);
				install_uplink_filter_rule_per_client(ext_prop, IPA_IP_v4, IPACM_Wan::getXlat_Mux_Id(), get_client_memptr(eth_client, eth_index)->mac);
				get_client_memptr(eth_client, eth_index)->ipv4_ul_rules_set = true;
			}
		}
		if(IPACM_Wan::isWanUP_V6(ipa_if_num))
		{
			if(IPACM_Wan::backhaul_is_sta_mode == false)
			{
				ext_prop = IPACM_Iface::ipacmcfg->GetExtProp(IPA_IP_v6);
				install_uplink_filter_rule_per_client(ext_prop, IPA_IP_v6, 0, get_client_memptr(eth_client, eth_index)->mac);
				get_client_memptr(eth_client, eth_index)->ipv6_ul_rules_set = true;
			}
		}
		handle_eth_client_route_rule_ext(get_client_memptr(eth_client, eth_index)->mac, IPA_IP_v4);
		handle_eth_client_route_rule_ext(get_client_memptr(eth_client, eth_index)->mac, IPA_IP_v6);
	}
	return IPACM_SUCCESS;
fail:
	/* Reset the mac from active list. */
	reset_active_lan_stats_index(get_client_memptr(eth_client, eth_index)->lan_stats_idx, mac_addr);
	/* Add the mac to inactive list. */
	get_free_inactive_lan_stats_index(mac_addr);
	get_client_memptr(eth_client, eth_index)->lan_stats_idx = -1;
	return IPACM_FAILURE;
}

int IPACM_Lan::handle_lan_client_disconnect(uint8_t *mac_addr)
{
	int i;
	uint8_t mac[IPA_MAC_ADDR_SIZE];

	/* Check if the client is in active list and remove it. */
	if (reset_active_lan_stats_index(get_lan_stats_index(mac_addr), mac_addr) == -1)
	{
		IPACMDBG_H("Failed to reset active lan_stats index, try inactive list. \n");
		/* If it is not in active list, check inactive list and remove it. */
		if (reset_inactive_lan_stats_index(mac_addr) == -1)
		{
			IPACMDBG_H("Failed to reserve inactive lan_stats index, return\n");
		}
		return IPACM_SUCCESS;
	}
	/* As we have free lan stats index. */
	/* Go through the inactive list and pick the first available one to add it to active list. */
	if (get_available_inactive_lan_client(mac) == IPACM_FAILURE)
	{
		IPACMDBG_H("Error in getting in active client.\n");
		return IPACM_FAILURE;
	}

	/* Add the mac to the active list. */
	if (get_free_active_lan_stats_index(mac) == -1)
	{
		IPACMDBG_H("Free active index not available. Abort\n");
		return IPACM_FAILURE;
	}

	/* Remove the mac from inactive list. */
	if (reset_inactive_lan_stats_index(mac) == IPACM_FAILURE)
	{
		IPACMDBG_H("Unable to remove the client from inactive list. Check\n");
	}

	/* Process the new lan stats index. */
	return handle_lan_client_connect(mac);
}


/*handle eth client routing rule with rule id*/
int IPACM_Lan::handle_eth_client_route_rule_ext(uint8_t *mac_addr, ipa_ip_type iptype)
{
	struct ipa_ioc_add_rt_rule_ext *rt_rule;
	struct ipa_rt_rule_add_ext *rt_rule_entry;
	uint32_t tx_index;
	int eth_index,v6_num;
	const int NUM = 1;
	ipacm_event_data_all data;

	if(tx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	IPACMDBG_H("Received mac_addr MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					 mac_addr[0], mac_addr[1], mac_addr[2],
					 mac_addr[3], mac_addr[4], mac_addr[5]);

	eth_index = get_eth_client_index(mac_addr);
	if (eth_index == IPACM_INVALID_INDEX)
	{
		IPACMDBG_H("eth client not found/attached \n");
		return IPACM_SUCCESS;
	}

	if (get_client_memptr(eth_client, eth_index)->lan_stats_idx == -1)
	{
		IPACMDBG_H("Lan client index not attached. \n");
		return IPACM_SUCCESS;
	}

	if (iptype==IPA_IP_v4) {
		IPACMDBG_H("eth client index: %d, ip-type: %d, ipv4_set:%d, ipv4_rule_set:%d \n", eth_index, iptype,
					 get_client_memptr(eth_client, eth_index)->ipv4_set,
					 get_client_memptr(eth_client, eth_index)->route_rule_set_v4);
	} else {
		IPACMDBG_H("eth client index: %d, ip-type: %d, ipv6_set:%d, ipv6_rule_num:%d \n", eth_index, iptype,
					 get_client_memptr(eth_client, eth_index)->ipv6_set,
					 get_client_memptr(eth_client, eth_index)->route_rule_set_v6);
	}
	/* Add default routing rules if not set yet */
	if ((iptype == IPA_IP_v4
			 && get_client_memptr(eth_client, eth_index)->route_rule_set_v4 == false
			 && get_client_memptr(eth_client, eth_index)->ipv4_set == true)
			|| (iptype == IPA_IP_v6
		            && get_client_memptr(eth_client, eth_index)->route_rule_set_v6 < get_client_memptr(eth_client, eth_index)->ipv6_set
					))
	{

        /* Add corresponding ipa_rm_resource_name of TX-endpoint up before IPV6 RT-rule set */
		IPACMDBG_H("dev %s add producer dependency\n", dev_name);
		if (tx_prop != NULL)
		{
			IPACMDBG_H("depend Got pipe %d rm index : %d \n", tx_prop->tx[0].dst_pipe, IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[tx_prop->tx[0].dst_pipe]);
			IPACM_Iface::ipacmcfg->AddRmDepend(IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[tx_prop->tx[0].dst_pipe],false);
		}
		rt_rule = (struct ipa_ioc_add_rt_rule_ext *)
			 calloc(1, sizeof(struct ipa_ioc_add_rt_rule_ext) +
						NUM * sizeof(struct ipa_rt_rule_add_ext));

		if (rt_rule == NULL)
		{
			PERROR("Error Locate ipa_ioc_add_rt_rule memory...\n");
			return IPACM_FAILURE;
		}

		rt_rule->commit = 1;
		rt_rule->num_rules = (uint8_t)NUM;
		rt_rule->ip = iptype;

		for (tx_index = 0; tx_index < iface_query->num_tx_props; tx_index++)
		{
			if(iptype != tx_prop->tx[tx_index].ip)
			{
				IPACMDBG_H("Tx:%d, ip-type: %d conflict ip-type: %d no RT-rule added\n",
						tx_index, tx_prop->tx[tx_index].ip,iptype);
				continue;
			}

			rt_rule_entry = &rt_rule->rules[0];
			rt_rule_entry->at_rear = 0;

			if (iptype == IPA_IP_v4)
			{
				IPACMDBG_H("client index(%d):ipv4 address: 0x%x\n", eth_index,
					get_client_memptr(eth_client, eth_index)->v4_addr);

				IPACMDBG_H("client(%d): v4 header handle:(0x%x)\n",
						 eth_index,
						 get_client_memptr(eth_client, eth_index)->hdr_hdl_v4);
				strlcpy(rt_rule->rt_tbl_name,
								IPACM_Iface::ipacmcfg->rt_tbl_lan_v4.name,
								sizeof(rt_rule->rt_tbl_name));
				rt_rule->rt_tbl_name[IPA_RESOURCE_NAME_MAX-1] = '\0';
				rt_rule_entry->rule.dst = tx_prop->tx[tx_index].dst_pipe;
				memcpy(&rt_rule_entry->rule.attrib,
						 &tx_prop->tx[tx_index].attrib,
						 sizeof(rt_rule_entry->rule.attrib));
				rt_rule_entry->rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
				rt_rule_entry->rule.hdr_hdl = get_client_memptr(eth_client, eth_index)->hdr_hdl_v4;
				rt_rule_entry->rule.attrib.u.v4.dst_addr = get_client_memptr(eth_client, eth_index)->v4_addr;
				rt_rule_entry->rule.attrib.u.v4.dst_addr_mask = 0xFFFFFFFF;
#ifdef FEATURE_IPA_V3
				rt_rule_entry->rule.hashable = false;
#endif
				rt_rule_entry->rule_id = 0;
				rt_rule_entry->rule_id = (get_client_memptr(eth_client, eth_index)->lan_stats_idx) | 0x200;
			    if (false == m_routing.AddRoutingRuleExt(rt_rule))
				{
					IPACMERR("Routing rule addition failed!\n");
					free(rt_rule);
					return IPACM_FAILURE;
			    }

			    /* copy ipv4 RT hdl */
		        get_client_memptr(eth_client, eth_index)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v4 =
				rt_rule->rules[0].rt_rule_hdl;
		        IPACMDBG_H("tx:%d, rt rule id=%x ip-type: %d\n", tx_index,
				rt_rule_entry->rule_id, iptype);

				get_client_memptr(eth_client, eth_index)->route_rule_set_v4 = true;
				/* Add NAT rules after ipv4 RT rules are set */ 		
				memset(&data, 0, sizeof(data));
				data.if_index = IPACM_Iface::ipacmcfg->iface_table[ipa_if_num].netlink_interface_index;
				data.iptype = IPA_IP_v4;
				data.ipv4_addr = get_client_memptr(eth_client, eth_index)->v4_addr;
				CtList->HandleNeighIpAddrAddEvt(&data);
			} else {

		        for(v6_num = get_client_memptr(eth_client, eth_index)->route_rule_set_v6;v6_num < get_client_memptr(eth_client, eth_index)->ipv6_set;v6_num++)
			    {
                    IPACMDBG_H("client(%d): v6 header handle:(0x%x)\n",
		  	    			 eth_index,
		  	    			 get_client_memptr(eth_client, eth_index)->hdr_hdl_v6);

		            /* v6 LAN_RT_TBL */
				strlcpy(rt_rule->rt_tbl_name,
			    					IPACM_Iface::ipacmcfg->rt_tbl_v6.name,
			    					sizeof(rt_rule->rt_tbl_name));
				rt_rule->rt_tbl_name[IPA_RESOURCE_NAME_MAX-1] = '\0';
		            /* Support QCMAP LAN traffic feature, send to A5 */
					rt_rule_entry->rule.dst = IPA_CLIENT_APPS_LAN_CONS;
			        memset(&rt_rule_entry->rule.attrib, 0, sizeof(rt_rule_entry->rule.attrib));
		   	        rt_rule_entry->rule.hdr_hdl = 0;
			        rt_rule_entry->rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[0] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][0];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[1] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][1];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[2] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][2];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[3] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][3];
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[0] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[1] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[2] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[3] = 0xFFFFFFFF;
#ifdef FEATURE_IPA_V3
					rt_rule_entry->rule.hashable = true;
#endif
					rt_rule_entry->rule_id = 0;
					if (false == m_routing.AddRoutingRuleExt(rt_rule))
					{
						IPACMERR("Routing rule addition failed!\n");
						free(rt_rule);
						return IPACM_FAILURE;
					}

		            get_client_memptr(eth_client, eth_index)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6[v6_num] = rt_rule->rules[0].rt_rule_hdl;
		            IPACMDBG_H("tx:%d, rt rule id=%x ip-type: %d\n", tx_index,
		            				 rt_rule_entry->rule_id, iptype);
			        /*Copy same rule to v6 WAN RT TBL*/
				strlcpy(rt_rule->rt_tbl_name, IPACM_Iface::ipacmcfg->rt_tbl_wan_v6.name, sizeof(rt_rule->rt_tbl_name));
				rt_rule->rt_tbl_name[IPA_RESOURCE_NAME_MAX-1] = '\0';
				/* Downlink traffic from Wan iface, directly through IPA */
					rt_rule_entry->rule.dst = tx_prop->tx[tx_index].dst_pipe;
			        memcpy(&rt_rule_entry->rule.attrib,
						 &tx_prop->tx[tx_index].attrib,
						 sizeof(rt_rule_entry->rule.attrib));
		   	        rt_rule_entry->rule.hdr_hdl = get_client_memptr(eth_client, eth_index)->hdr_hdl_v6;
			        rt_rule_entry->rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[0] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][0];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[1] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][1];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[2] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][2];
		   	        rt_rule_entry->rule.attrib.u.v6.dst_addr[3] = get_client_memptr(eth_client, eth_index)->v6_addr[v6_num][3];
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[0] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[1] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[2] = 0xFFFFFFFF;
					rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[3] = 0xFFFFFFFF;
#ifdef FEATURE_IPA_V3
					rt_rule_entry->rule.hashable = true;
#endif
					rt_rule_entry->rule_id = get_client_memptr(eth_client, eth_index)->lan_stats_idx | 0x200;
		            if (false == m_routing.AddRoutingRuleExt(rt_rule))
		            {
							IPACMERR("Routing rule addition failed!\n");
							free(rt_rule);
							return IPACM_FAILURE;
		            }

		            get_client_memptr(eth_client, eth_index)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6_wan[v6_num] = rt_rule->rules[0].rt_rule_hdl;
					IPACMDBG_H("tx:%d, rt rule id=%x ip-type: %d\n", tx_index,
		            				 rt_rule_entry->rule_id, iptype);
				}
				get_client_memptr(eth_client, eth_index)->route_rule_set_v6 = get_client_memptr(eth_client, eth_index)->ipv6_set;
			} /* end of for loop */
			free(rt_rule);
		}
	}
	return IPACM_SUCCESS;
}
#endif

/* handle odu client initial, construct full headers (tx property) */
int IPACM_Lan::handle_odu_hdr_init(uint8_t *mac_addr)
{
	int res = IPACM_SUCCESS, len = 0;
	struct ipa_ioc_copy_hdr sCopyHeader;
	struct ipa_ioc_add_hdr *pHeaderDescriptor = NULL;
	uint32_t cnt;

	IPACMDBG("Received Client MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					 mac_addr[0], mac_addr[1], mac_addr[2],
					 mac_addr[3], mac_addr[4], mac_addr[5]);

	/* add header to IPA */
	if(tx_prop != NULL)
	{
		len = sizeof(struct ipa_ioc_add_hdr) + (1 * sizeof(struct ipa_hdr_add));
		pHeaderDescriptor = (struct ipa_ioc_add_hdr *)calloc(1, len);
		if (pHeaderDescriptor == NULL)
		{
			IPACMERR("calloc failed to allocate pHeaderDescriptor\n");
			return IPACM_FAILURE;
		}

		/* copy partial header for v4*/
		for (cnt=0; cnt<tx_prop->num_tx_props; cnt++)
		{
				 if(tx_prop->tx[cnt].ip==IPA_IP_v4)
				 {
								IPACMDBG("Got partial v4-header name from %d tx props\n", cnt);
								memset(&sCopyHeader, 0, sizeof(sCopyHeader));
								memcpy(sCopyHeader.name,
											tx_prop->tx[cnt].hdr_name,
											 sizeof(sCopyHeader.name));
								IPACMDBG("header name: %s in tx:%d\n", sCopyHeader.name,cnt);
								if (m_header.CopyHeader(&sCopyHeader) == false)
								{
									PERROR("ioctl copy header failed");
									res = IPACM_FAILURE;
									goto fail;
								}
								IPACMDBG("header length: %d, paritial: %d\n", sCopyHeader.hdr_len, sCopyHeader.is_partial);
								if (sCopyHeader.hdr_len > IPA_HDR_MAX_SIZE)
								{
									IPACMERR("header oversize\n");
									res = IPACM_FAILURE;
									goto fail;
								}
								else
								{
									memcpy(pHeaderDescriptor->hdr[0].hdr,
												 sCopyHeader.hdr,
												 sCopyHeader.hdr_len);
								}
								/* copy client mac_addr to partial header */
								if (sCopyHeader.is_eth2_ofst_valid)
								{
									memcpy(&pHeaderDescriptor->hdr[0].hdr[sCopyHeader.eth2_ofst],
											 mac_addr,
											 IPA_MAC_ADDR_SIZE);
								}
								/* replace src mac to bridge mac_addr if any  */
								if (IPACM_Iface::ipacmcfg->ipa_bridge_enable)
								{
									memcpy(&pHeaderDescriptor->hdr[0].hdr[sCopyHeader.eth2_ofst+IPA_MAC_ADDR_SIZE],
											IPACM_Iface::ipacmcfg->bridge_mac,
											IPA_MAC_ADDR_SIZE);
									IPACMDBG_H("device is in bridge mode \n");
								}

								pHeaderDescriptor->commit = true;
								pHeaderDescriptor->num_hdrs = 1;

								memset(pHeaderDescriptor->hdr[0].name, 0,
											 sizeof(pHeaderDescriptor->hdr[0].name));
								strlcpy(pHeaderDescriptor->hdr[0].name, IPA_ODU_HDR_NAME_v4, sizeof(pHeaderDescriptor->hdr[0].name));
								pHeaderDescriptor->hdr[0].hdr_len = sCopyHeader.hdr_len;
								pHeaderDescriptor->hdr[0].hdr_hdl = -1;
								pHeaderDescriptor->hdr[0].is_partial = 0;
								pHeaderDescriptor->hdr[0].status = -1;

					 if (m_header.AddHeader(pHeaderDescriptor) == false ||
							pHeaderDescriptor->hdr[0].status != 0)
					 {
						IPACMERR("ioctl IPA_IOC_ADD_HDR failed: %d\n", pHeaderDescriptor->hdr[0].status);
						res = IPACM_FAILURE;
						goto fail;
					 }

					ODU_hdr_hdl_v4 = pHeaderDescriptor->hdr[0].hdr_hdl;
					ipv4_header_set = true ;
					IPACMDBG(" ODU v4 full header name:%s header handle:(0x%x)\n",
										 pHeaderDescriptor->hdr[0].name,
												 ODU_hdr_hdl_v4);
					break;
				 }
		}


		/* copy partial header for v6*/
		for (cnt=0; cnt<tx_prop->num_tx_props; cnt++)
		{
			if(tx_prop->tx[cnt].ip==IPA_IP_v6)
			{

				IPACMDBG("Got partial v6-header name from %d tx props\n", cnt);
				memset(&sCopyHeader, 0, sizeof(sCopyHeader));
				memcpy(sCopyHeader.name,
						tx_prop->tx[cnt].hdr_name,
							sizeof(sCopyHeader.name));

				IPACMDBG("header name: %s in tx:%d\n", sCopyHeader.name,cnt);
				if (m_header.CopyHeader(&sCopyHeader) == false)
				{
					PERROR("ioctl copy header failed");
					res = IPACM_FAILURE;
					goto fail;
				}

				IPACMDBG("header length: %d, paritial: %d\n", sCopyHeader.hdr_len, sCopyHeader.is_partial);
				if (sCopyHeader.hdr_len > IPA_HDR_MAX_SIZE)
				{
					IPACMERR("header oversize\n");
					res = IPACM_FAILURE;
					goto fail;
				}
				else
				{
					memcpy(pHeaderDescriptor->hdr[0].hdr,
							sCopyHeader.hdr,
								sCopyHeader.hdr_len);
				}

				/* copy client mac_addr to partial header */
				if (sCopyHeader.is_eth2_ofst_valid)
				{
					memcpy(&pHeaderDescriptor->hdr[0].hdr[sCopyHeader.eth2_ofst],
					 mac_addr,
					 IPA_MAC_ADDR_SIZE);
				}
				/* replace src mac to bridge mac_addr if any  */
				if (IPACM_Iface::ipacmcfg->ipa_bridge_enable)
				{
					memcpy(&pHeaderDescriptor->hdr[0].hdr[sCopyHeader.eth2_ofst+IPA_MAC_ADDR_SIZE],
							IPACM_Iface::ipacmcfg->bridge_mac,
							IPA_MAC_ADDR_SIZE);
					IPACMDBG_H("device is in bridge mode \n");
				}

				pHeaderDescriptor->commit = true;
				pHeaderDescriptor->num_hdrs = 1;

				memset(pHeaderDescriptor->hdr[0].name, 0,
					 sizeof(pHeaderDescriptor->hdr[0].name));

				strlcpy(pHeaderDescriptor->hdr[0].name, IPA_ODU_HDR_NAME_v6, sizeof(pHeaderDescriptor->hdr[0].name));
				pHeaderDescriptor->hdr[0].hdr_len = sCopyHeader.hdr_len;
				pHeaderDescriptor->hdr[0].hdr_hdl = -1;
				pHeaderDescriptor->hdr[0].is_partial = 0;
				pHeaderDescriptor->hdr[0].status = -1;

				if (m_header.AddHeader(pHeaderDescriptor) == false ||
						pHeaderDescriptor->hdr[0].status != 0)
				{
					IPACMERR("ioctl IPA_IOC_ADD_HDR failed: %d\n", pHeaderDescriptor->hdr[0].status);
					res = IPACM_FAILURE;
					goto fail;
				}

				ODU_hdr_hdl_v6 = pHeaderDescriptor->hdr[0].hdr_hdl;
				ipv6_header_set = true ;
				IPACMDBG(" ODU v4 full header name:%s header handle:(0x%x)\n",
									 pHeaderDescriptor->hdr[0].name,
											 ODU_hdr_hdl_v6);
				break;
			}
		}
	}
fail:
	free(pHeaderDescriptor);
	return res;
}


/* handle odu default route rule configuration */
int IPACM_Lan::handle_odu_route_add()
{
	/* add default WAN route */
	struct ipa_ioc_add_rt_rule *rt_rule;
	struct ipa_rt_rule_add *rt_rule_entry;
	uint32_t tx_index;
	const int NUM = 1;

	if(tx_prop == NULL)
	{
	  IPACMDBG_H("No tx properties, ignore default route setting\n");
	  return IPACM_SUCCESS;
	}

	rt_rule = (struct ipa_ioc_add_rt_rule *)
		 calloc(1, sizeof(struct ipa_ioc_add_rt_rule) +
						NUM * sizeof(struct ipa_rt_rule_add));

	if (!rt_rule)
	{
		IPACMERR("Error Locate ipa_ioc_add_rt_rule memory...\n");
		return IPACM_FAILURE;
	}

	rt_rule->commit = 1;
	rt_rule->num_rules = (uint8_t)NUM;


	IPACMDBG_H("WAN table created %s \n", rt_rule->rt_tbl_name);
	rt_rule_entry = &rt_rule->rules[0];
	rt_rule_entry->at_rear = true;

	for (tx_index = 0; tx_index < iface_query->num_tx_props; tx_index++)
	{

		if (IPA_IP_v4 == tx_prop->tx[tx_index].ip)
		{
			strlcpy(rt_rule->rt_tbl_name, IPACM_Iface::ipacmcfg->rt_tbl_odu_v4.name, sizeof(rt_rule->rt_tbl_name));
			rt_rule_entry->rule.hdr_hdl = ODU_hdr_hdl_v4;
			rt_rule->ip = IPA_IP_v4;
		}
		else
		{
			strlcpy(rt_rule->rt_tbl_name, IPACM_Iface::ipacmcfg->rt_tbl_odu_v6.name, sizeof(rt_rule->rt_tbl_name));
			rt_rule_entry->rule.hdr_hdl = ODU_hdr_hdl_v6;
			rt_rule->ip = IPA_IP_v6;
		}

		rt_rule_entry->rule.dst = tx_prop->tx[tx_index].dst_pipe;
		memcpy(&rt_rule_entry->rule.attrib,
					 &tx_prop->tx[tx_index].attrib,
					 sizeof(rt_rule_entry->rule.attrib));

		rt_rule_entry->rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
		if (IPA_IP_v4 == tx_prop->tx[tx_index].ip)
		{
			rt_rule_entry->rule.attrib.u.v4.dst_addr      = 0;
			rt_rule_entry->rule.attrib.u.v4.dst_addr_mask = 0;
#ifdef FEATURE_IPA_V3
			rt_rule_entry->rule.hashable = true;
#endif
			if (false == m_routing.AddRoutingRule(rt_rule))
			{
				IPACMERR("Routing rule addition failed!\n");
				free(rt_rule);
				return IPACM_FAILURE;
			}
			odu_route_rule_v4_hdl[tx_index] = rt_rule_entry->rt_rule_hdl;
			IPACMDBG_H("Got ipv4 ODU-route rule hdl:0x%x,tx:%d,ip-type: %d \n",
						 odu_route_rule_v4_hdl[tx_index],
						 tx_index,
						 IPA_IP_v4);
		}
		else
		{
			rt_rule_entry->rule.attrib.u.v6.dst_addr[0] = 0;
			rt_rule_entry->rule.attrib.u.v6.dst_addr[1] = 0;
			rt_rule_entry->rule.attrib.u.v6.dst_addr[2] = 0;
			rt_rule_entry->rule.attrib.u.v6.dst_addr[3] = 0;
			rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[0] = 0;
			rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[1] = 0;
			rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[2] = 0;
			rt_rule_entry->rule.attrib.u.v6.dst_addr_mask[3] = 0;
#ifdef FEATURE_IPA_V3
			rt_rule_entry->rule.hashable = true;
#endif
			if (false == m_routing.AddRoutingRule(rt_rule))
			{
				IPACMERR("Routing rule addition failed!\n");
				free(rt_rule);
				return IPACM_FAILURE;
			}
			odu_route_rule_v6_hdl[tx_index] = rt_rule_entry->rt_rule_hdl;
			IPACMDBG_H("Set ipv6 ODU-route rule hdl for v6_lan_table:0x%x,tx:%d,ip-type: %d \n",
					odu_route_rule_v6_hdl[tx_index],
					tx_index,
					IPA_IP_v6);
		}
	}
	free(rt_rule);
	return IPACM_SUCCESS;
}

/* handle odu default route rule deletion */
int IPACM_Lan::handle_odu_route_del()
{
	uint32_t tx_index;

	if(tx_prop == NULL)
	{
		IPACMDBG_H("No tx properties, ignore delete default route setting\n");
		return IPACM_SUCCESS;
	}

	for (tx_index = 0; tx_index < iface_query->num_tx_props; tx_index++)
	{
		if (tx_prop->tx[tx_index].ip == IPA_IP_v4)
		{
			IPACMDBG_H("Tx:%d, ip-type: %d match ip-type: %d, RT-rule deleted\n",
					tx_index, tx_prop->tx[tx_index].ip,IPA_IP_v4);

			if (m_routing.DeleteRoutingHdl(odu_route_rule_v4_hdl[tx_index], IPA_IP_v4)
					== false)
			{
				IPACMERR("IP-family:%d, Routing rule(hdl:0x%x) deletion failed with tx_index %d!\n", IPA_IP_v4, odu_route_rule_v4_hdl[tx_index], tx_index);
				return IPACM_FAILURE;
			}
		}
		else
		{
			IPACMDBG_H("Tx:%d, ip-type: %d match ip-type: %d, RT-rule deleted\n",
					tx_index, tx_prop->tx[tx_index].ip,IPA_IP_v6);

			if (m_routing.DeleteRoutingHdl(odu_route_rule_v6_hdl[tx_index], IPA_IP_v6)
					== false)
			{
				IPACMERR("IP-family:%d, Routing rule(hdl:0x%x) deletion failed with tx_index %d!\n", IPA_IP_v6, odu_route_rule_v6_hdl[tx_index], tx_index);
				return IPACM_FAILURE;
			}
		}
	}

	return IPACM_SUCCESS;
}

/*handle eth client del mode*/
int IPACM_Lan::handle_eth_client_down_evt(uint8_t *mac_addr)
{
	int clt_indx;
	uint32_t tx_index;
	int num_eth_client_tmp = num_eth_client;
	int num_v6;
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	struct wan_ioctl_lan_client_info *client_info;
#endif

	IPACMDBG_H("total client: %d\n", num_eth_client_tmp);

	clt_indx = get_eth_client_index(mac_addr);
	if (clt_indx == IPACM_INVALID_INDEX)
	{
		IPACMDBG_H("eth client not attached\n");
		return IPACM_SUCCESS;
	}

	/* First reset nat rules and then route rules */
	if(get_client_memptr(eth_client, clt_indx)->ipv4_set == true)
	{
			IPACMDBG_H("Clean Nat Rules for ipv4:0x%x\n", get_client_memptr(eth_client, clt_indx)->v4_addr);
			CtList->HandleNeighIpAddrDelEvt(get_client_memptr(eth_client, clt_indx)->v4_addr);
 	}

	if (delete_eth_rtrules(clt_indx, IPA_IP_v4))
	{
		IPACMERR("unbale to delete ecm-client v4 route rules for index: %d\n", clt_indx);
		return IPACM_FAILURE;
	}

	if (delete_eth_rtrules(clt_indx, IPA_IP_v6))
	{
		IPACMERR("unbale to delete ecm-client v6 route rules for index: %d\n", clt_indx);
		return IPACM_FAILURE;
	}

	/* Delete eth client header */
	if(get_client_memptr(eth_client, clt_indx)->ipv4_header_set == true)
	{
		if (m_header.DeleteHeaderHdl(get_client_memptr(eth_client, clt_indx)->hdr_hdl_v4)
				== false)
		{
			return IPACM_FAILURE;
		}
		get_client_memptr(eth_client, clt_indx)->ipv4_header_set = false;
	}

	if(get_client_memptr(eth_client, clt_indx)->ipv6_header_set == true)
	{
		if (m_header.DeleteHeaderHdl(get_client_memptr(eth_client, clt_indx)->hdr_hdl_v6)
				== false)
		{
			return IPACM_FAILURE;
		}
		get_client_memptr(eth_client, clt_indx)->ipv6_header_set = false;
	}

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	if (get_client_memptr(eth_client, clt_indx)->ipv4_ul_rules_set == true)
	{
		if (delete_uplink_filter_rule_per_client(IPA_IP_v4, get_client_memptr(eth_client, clt_indx)->mac))
		{
			IPACMERR("unbale to delete uplink v4 filter rules for index: %d\n", clt_indx);
			return IPACM_FAILURE;
		}
	}

	if (get_client_memptr(eth_client, clt_indx)->ipv6_ul_rules_set == true)
	{
		if (delete_uplink_filter_rule_per_client(IPA_IP_v6, get_client_memptr(eth_client, clt_indx)->mac))
		{
			IPACMERR("unbale to delete uplink v6 filter rules for index: %d\n", clt_indx);
			return IPACM_FAILURE;
		}
	}
#endif

	/* Reset ip_set to 0*/
	get_client_memptr(eth_client, clt_indx)->ipv4_set = false;
	get_client_memptr(eth_client, clt_indx)->ipv6_set = 0;
	get_client_memptr(eth_client, clt_indx)->ipv4_header_set = false;
	get_client_memptr(eth_client, clt_indx)->ipv6_header_set = false;
	get_client_memptr(eth_client, clt_indx)->route_rule_set_v4 = false;
	get_client_memptr(eth_client, clt_indx)->route_rule_set_v6 = 0;
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	get_client_memptr(eth_client, clt_indx)->ipv4_ul_rules_set = false;
	get_client_memptr(eth_client, clt_indx)->ipv6_ul_rules_set = false;
	if (get_client_memptr(eth_client, clt_indx)->lan_stats_idx != -1)
	{
		/* Clear the lan client info. */
		client_info = (struct wan_ioctl_lan_client_info *)malloc(sizeof(struct wan_ioctl_lan_client_info));
		if (client_info == NULL)
		{
			IPACMERR("Unable to allocate memory\n");
			return IPACM_FAILURE;
		}
		memset(client_info, 0, sizeof(struct wan_ioctl_lan_client_info));
		if (ipa_if_cate == LAN_IF)
		{
			client_info->device_type = IPACM_CLIENT_DEVICE_TYPE_USB;
		}
		else if (ipa_if_cate == ODU_IF)
		{
			client_info->device_type = IPACM_CLIENT_DEVICE_TYPE_ETH;
		}
		memcpy(client_info->mac,
				get_client_memptr(eth_client, clt_indx)->mac,
				IPA_MAC_ADDR_SIZE);
		client_info->client_init = 0;
		client_info->client_idx = get_client_memptr(eth_client, clt_indx)->lan_stats_idx;
		client_info->ul_src_pipe = IPA_CLIENT_MAX;
		if (rx_prop)
		{
			client_info->ul_src_pipe = rx_prop->rx[0].src_pipe;
		}
		clear_lan_client_info(client_info);
		free(client_info);
	}
	get_client_memptr(eth_client, clt_indx)->lan_stats_idx = -1;
	memset(get_client_memptr(eth_client, clt_indx)->wan_ul_fl_rule_hdl_v4, 0, MAX_WAN_UL_FILTER_RULES * sizeof(uint32_t));
	memset(get_client_memptr(eth_client, clt_indx)->wan_ul_fl_rule_hdl_v6, 0, MAX_WAN_UL_FILTER_RULES * sizeof(uint32_t));
#endif

	for (; clt_indx < num_eth_client_tmp - 1; clt_indx++)
	{
		memcpy(get_client_memptr(eth_client, clt_indx)->mac,
					 get_client_memptr(eth_client, (clt_indx + 1))->mac,
					 sizeof(get_client_memptr(eth_client, clt_indx)->mac));

		get_client_memptr(eth_client, clt_indx)->hdr_hdl_v4 = get_client_memptr(eth_client, (clt_indx + 1))->hdr_hdl_v4;
		get_client_memptr(eth_client, clt_indx)->hdr_hdl_v6 = get_client_memptr(eth_client, (clt_indx + 1))->hdr_hdl_v6;
		get_client_memptr(eth_client, clt_indx)->v4_addr = get_client_memptr(eth_client, (clt_indx + 1))->v4_addr;

		get_client_memptr(eth_client, clt_indx)->ipv4_set = get_client_memptr(eth_client, (clt_indx + 1))->ipv4_set;
		get_client_memptr(eth_client, clt_indx)->ipv6_set = get_client_memptr(eth_client, (clt_indx + 1))->ipv6_set;
		get_client_memptr(eth_client, clt_indx)->ipv4_header_set = get_client_memptr(eth_client, (clt_indx + 1))->ipv4_header_set;
		get_client_memptr(eth_client, clt_indx)->ipv6_header_set = get_client_memptr(eth_client, (clt_indx + 1))->ipv6_header_set;

		get_client_memptr(eth_client, clt_indx)->route_rule_set_v4 = get_client_memptr(eth_client, (clt_indx + 1))->route_rule_set_v4;
		get_client_memptr(eth_client, clt_indx)->route_rule_set_v6 = get_client_memptr(eth_client, (clt_indx + 1))->route_rule_set_v6;

        for (num_v6=0;num_v6< get_client_memptr(eth_client, clt_indx)->ipv6_set;num_v6++)
	    {
		    get_client_memptr(eth_client, clt_indx)->v6_addr[num_v6][0] = get_client_memptr(eth_client, (clt_indx + 1))->v6_addr[num_v6][0];
		    get_client_memptr(eth_client, clt_indx)->v6_addr[num_v6][1] = get_client_memptr(eth_client, (clt_indx + 1))->v6_addr[num_v6][1];
		    get_client_memptr(eth_client, clt_indx)->v6_addr[num_v6][2] = get_client_memptr(eth_client, (clt_indx + 1))->v6_addr[num_v6][2];
		    get_client_memptr(eth_client, clt_indx)->v6_addr[num_v6][3] = get_client_memptr(eth_client, (clt_indx + 1))->v6_addr[num_v6][3];
        }

		for (tx_index = 0; tx_index < iface_query->num_tx_props; tx_index++)
		{
			get_client_memptr(eth_client, clt_indx)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v4 =
				 get_client_memptr(eth_client, (clt_indx + 1))->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v4;

			for(num_v6=0;num_v6< get_client_memptr(eth_client, clt_indx)->route_rule_set_v6;num_v6++)
			{
			  get_client_memptr(eth_client, clt_indx)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6[num_v6] =
			   	 get_client_memptr(eth_client, (clt_indx + 1))->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6[num_v6];
			  get_client_memptr(eth_client, clt_indx)->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6_wan[num_v6] =
			   	 get_client_memptr(eth_client, (clt_indx + 1))->eth_rt_hdl[tx_index].eth_rt_rule_hdl_v6_wan[num_v6];
		    }
		}
	}

	IPACMDBG_H(" %d eth client deleted successfully \n", num_eth_client);
	num_eth_client = num_eth_client - 1;
	IPACMDBG_H(" Number of eth client: %d\n", num_eth_client);

	/* Del RM dependency */
	if(num_eth_client == 0)
	{
		/* Delete corresponding ipa_rm_resource_name of TX-endpoint after delete all IPV4V6 RT-rule*/
		IPACMDBG_H("dev %s add producer dependency\n", dev_name);
		if (tx_prop != NULL)
		{
			IPACMDBG_H("depend Got pipe %d rm index : %d \n", tx_prop->tx[0].dst_pipe, IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[tx_prop->tx[0].dst_pipe]);
			IPACM_Iface::ipacmcfg->DelRmDepend(IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[tx_prop->tx[0].dst_pipe]);
		}
	}

	return IPACM_SUCCESS;
}

/*handle LAN iface down event*/
int IPACM_Lan::handle_down_evt()
{
	int i;
	int res = IPACM_SUCCESS;
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	struct wan_ioctl_lan_client_info *client_info;
#endif

	IPACMDBG_H("lan handle_down_evt\n ");
	if (ipa_if_cate == ODU_IF)
	{
		/* delete ODU default RT rules */
		if (IPACM_Iface::ipacmcfg->ipacm_odu_embms_enable == true)
		{
			IPACMDBG_H("eMBMS enable, delete eMBMS DL RT rule\n");
			handle_odu_route_del();
		}

		/* delete full header */
		if (ipv4_header_set)
		{
			if (m_header.DeleteHeaderHdl(ODU_hdr_hdl_v4)
					== false)
			{
					IPACMERR("ODU ipv4 header delete fail\n");
					res = IPACM_FAILURE;
					goto fail;
			}
			IPACMDBG_H("ODU ipv4 header delete success\n");
		}

		if (ipv6_header_set)
		{
			if (m_header.DeleteHeaderHdl(ODU_hdr_hdl_v6)
					== false)
			{
				IPACMERR("ODU ipv6 header delete fail\n");
				res = IPACM_FAILURE;
				goto fail;
			}
			IPACMERR("ODU ipv6 header delete success\n");
		}
	}

	/* no iface address up, directly close iface*/
	if (ip_type == IPACM_IP_NULL)
	{
		goto fail;
	}

	/* delete wan filter rule */
	if (IPACM_Wan::isWanUP(ipa_if_num) && rx_prop != NULL)
	{
		IPACMDBG_H("LAN IF goes down, backhaul type %d\n", IPACM_Wan::backhaul_is_sta_mode);
		handle_wan_down(IPACM_Wan::backhaul_is_sta_mode);
	}

	if (IPACM_Wan::isWanUP_V6(ipa_if_num) && rx_prop != NULL)
	{
		IPACMDBG_H("LAN IF goes down, backhaul type %d\n", IPACM_Wan::backhaul_is_sta_mode);
		handle_wan_down_v6(IPACM_Wan::backhaul_is_sta_mode);
	}

	/* delete default filter rules */
	if (ip_type != IPA_IP_v6 && rx_prop != NULL)
	{
		if(m_filtering.DeleteFilteringHdls(ipv4_icmp_flt_rule_hdl, IPA_IP_v4, NUM_IPV4_ICMP_FLT_RULE) == false)
		{
			IPACMERR("Error Deleting ICMPv4 Filtering Rule, aborting...\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4, NUM_IPV4_ICMP_FLT_RULE);

		if(dft_v4fl_rule_hdl[0] != 0)
		{
				if (m_filtering.DeleteFilteringHdls(dft_v4fl_rule_hdl, IPA_IP_v4,
						IPV4_DEFAULT_FILTERTING_RULES) == false)
				{
					IPACMERR("Error Deleting Filtering Rule, aborting...\n");
					res = IPACM_FAILURE;
					goto fail;
				}
				IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4, IPV4_DEFAULT_FILTERTING_RULES);
		}

		/* free private-subnet ipv4 filter rules */
		if (IPACM_Iface::ipacmcfg->ipa_num_private_subnet > IPA_PRIV_SUBNET_FILTER_RULE_HANDLES)
		{
			IPACMERR(" the number of rules are bigger than array, aborting...\n");
			res = IPACM_FAILURE;
			goto fail;
		}

#ifdef FEATURE_IPA_ANDROID
		if(m_filtering.DeleteFilteringHdls(private_fl_rule_hdl, IPA_IP_v4, IPA_MAX_PRIVATE_SUBNET_ENTRIES) == false)
		{
			IPACMERR("Error deleting private subnet IPv4 flt rules.\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4, IPA_MAX_PRIVATE_SUBNET_ENTRIES);
#else
		if (m_filtering.DeleteFilteringHdls(private_fl_rule_hdl, IPA_IP_v4, IPACM_Iface::ipacmcfg->ipa_num_private_subnet) == false)
		{
			IPACMERR("Error deleting private subnet IPv4 flt rules.\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4, IPACM_Iface::ipacmcfg->ipa_num_private_subnet);
#endif
		IPACMDBG_H("Deleted private subnet v4 filter rules successfully.\n");
	}
	IPACMDBG_H("Finished delete default iface ipv4 filtering rules \n ");

	if (ip_type != IPA_IP_v4 && rx_prop != NULL)
	{
		if(m_filtering.DeleteFilteringHdls(ipv6_icmp_flt_rule_hdl, IPA_IP_v6, NUM_IPV6_ICMP_FLT_RULE) == false)
		{
			IPACMERR("Error Deleting ICMPv6 Filtering Rule, aborting...\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, NUM_IPV6_ICMP_FLT_RULE);

		if (dft_v6fl_rule_hdl[0] != 0)
		{
			if (m_filtering.DeleteFilteringHdls(dft_v6fl_rule_hdl, IPA_IP_v6, IPV6_DEFAULT_FILTERTING_RULES) == false)
			{
				IPACMERR("Error Adding RuleTable(1) to Filtering, aborting...\n");
				res = IPACM_FAILURE;
				goto fail;
			}
				IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, IPV6_DEFAULT_FILTERTING_RULES);
		}
#ifdef FEATURE_L2TP
		if(ipa_if_cate == ODU_IF)
		{
			if(m_filtering.DeleteFilteringHdls(tcp_syn_flt_rule_hdl, IPA_IP_v6, IPA_IP_MAX) == false)
			{
				IPACMERR("Error Deleting TCP SYN L2TP Filtering Rule, aborting...\n");
				res = IPACM_FAILURE;
				goto fail;
			}
		}
#endif
	}
	IPACMDBG_H("Finished delete default iface ipv6 filtering rules \n ");

	if (ip_type != IPA_IP_v6)
	{
		if (m_routing.DeleteRoutingHdl(dft_rt_rule_hdl[0], IPA_IP_v4)
				== false)
		{
			IPACMERR("Routing rule deletion failed!\n");
			res = IPACM_FAILURE;
			goto fail;
		}
	}
	IPACMDBG_H("Finished delete default iface ipv4 rules \n ");

	/* delete default v6 routing rule */
	if (ip_type != IPA_IP_v4)
	{
		/* may have multiple ipv6 iface-RT rules*/
		for (i = 0; i < 2*num_dft_rt_v6; i++)
		{
			if (m_routing.DeleteRoutingHdl(dft_rt_rule_hdl[MAX_DEFAULT_v4_ROUTE_RULES + i], IPA_IP_v6)
					== false)
			{
				IPACMERR("Routing rule deletion failed!\n");
				res = IPACM_FAILURE;
				goto fail;
			}
		}
	}

	IPACMDBG_H("Finished delete default iface ipv6 rules \n ");

	/* free the edm clients cache */
	IPACMDBG_H("Free ecm clients cache\n");

	/* Delete corresponding ipa_rm_resource_name of TX-endpoint after delete all IPV4V6 RT-rule */
	IPACMDBG_H("dev %s delete producer dependency\n", dev_name);
	if (tx_prop != NULL)
	{
		IPACMDBG_H("depend Got pipe %d rm index : %d \n", tx_prop->tx[0].dst_pipe, IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[tx_prop->tx[0].dst_pipe]);
		IPACM_Iface::ipacmcfg->DelRmDepend(IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[tx_prop->tx[0].dst_pipe]);
	}

	eth_bridge_post_event(IPA_ETH_BRIDGE_IFACE_DOWN, IPA_IP_MAX, NULL, NULL, NULL);

/* Delete private subnet*/
#ifdef FEATURE_IPA_ANDROID
	if (ip_type != IPA_IP_v6)
	{
		IPACMDBG_H("current IPACM private subnet_addr number(%d)\n", IPACM_Iface::ipacmcfg->ipa_num_private_subnet);
		IPACMDBG_H(" Delete IPACM private subnet_addr as: 0x%x \n", if_ipv4_subnet);
		if(IPACM_Iface::ipacmcfg->DelPrivateSubnet(if_ipv4_subnet, ipa_if_num) == false)
		{
			IPACMERR(" can't Delete IPACM private subnet_addr as: 0x%x \n", if_ipv4_subnet);
		}
	}

	/* reset the IPA-client pipe enum */
	if(ipa_if_cate != WAN_IF)
	{
		handle_tethering_client(true, IPACM_CLIENT_USB);
	}
#endif /* defined(FEATURE_IPA_ANDROID)*/
fail:
	/* clean eth-client header, routing rules */
	IPACMDBG_H("left %d eth clients need to be deleted \n ", num_eth_client);
	for (i = 0; i < num_eth_client; i++)
	{
		/* First reset nat rules and then route rules */
		if(get_client_memptr(eth_client, i)->ipv4_set == true)
		{
			IPACMDBG_H("Clean Nat Rules for ipv4:0x%x\n", get_client_memptr(eth_client, i)->v4_addr);
			CtList->HandleNeighIpAddrDelEvt(get_client_memptr(eth_client, i)->v4_addr);
		}

		if (delete_eth_rtrules(i, IPA_IP_v4))
		{
			IPACMERR("unbale to delete ecm-client v4 route rules for index %d\n", i);
			res = IPACM_FAILURE;
		}

		if (delete_eth_rtrules(i, IPA_IP_v6))
		{
			IPACMERR("unbale to delete ecm-client v6 route rules for index %d\n", i);
			res = IPACM_FAILURE;
		}

		IPACMDBG_H("Delete %d client header\n", num_eth_client);

		if(get_client_memptr(eth_client, i)->ipv4_header_set == true)
		{
			if (m_header.DeleteHeaderHdl(get_client_memptr(eth_client, i)->hdr_hdl_v4)
				== false)
			{
				res = IPACM_FAILURE;
			}
		}

		if(get_client_memptr(eth_client, i)->ipv6_header_set == true)
		{
			if (m_header.DeleteHeaderHdl(get_client_memptr(eth_client, i)->hdr_hdl_v6)
					== false)
			{
				res = IPACM_FAILURE;
			}
		}

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
		if (get_client_memptr(eth_client, i)->lan_stats_idx != -1)
		{
			/* Clear the lan client info. */
			client_info = (struct wan_ioctl_lan_client_info *)malloc(sizeof(struct wan_ioctl_lan_client_info));
			if (client_info == NULL)
			{
				IPACMERR("Unable to allocate memory\n");
				res = IPACM_FAILURE;
			}
			else
			{
				memset(client_info, 0, sizeof(struct wan_ioctl_lan_client_info));
				if (ipa_if_cate == LAN_IF)
				{
					client_info->device_type = IPACM_CLIENT_DEVICE_TYPE_USB;
				}
				else if (ipa_if_cate == ODU_IF)
				{
					client_info->device_type = IPACM_CLIENT_DEVICE_TYPE_ETH;
				}
				memcpy(client_info->mac,
						get_client_memptr(eth_client, i)->mac,
						IPA_MAC_ADDR_SIZE);
				client_info->client_init = 0;
				client_info->client_idx = get_client_memptr(eth_client, i)->lan_stats_idx;
				client_info->ul_src_pipe = IPA_CLIENT_MAX;
				if (rx_prop)
				{
					client_info->ul_src_pipe = rx_prop->rx[0].src_pipe;
				}
				if (clear_lan_client_info(client_info))
				{
					res = IPACM_FAILURE;
				}
				free(client_info);
			}
			get_client_memptr(eth_client, i)->lan_stats_idx = -1;
		}
#endif
	} /* end of for loop */

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
		/* Reset the lan stats indices belonging to this object. */
		if (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
		{
			IPACMDBG_H("Resetting lan stats indices. \n");
			reset_lan_stats_index();
		}
#endif

	/* check software routing fl rule hdl */
	if (softwarerouting_act == true && rx_prop != NULL)
	{
		handle_software_routing_disable();
	}

	if (odu_route_rule_v4_hdl != NULL)
	{
		free(odu_route_rule_v4_hdl);
	}
	if (odu_route_rule_v6_hdl != NULL)
	{
		free(odu_route_rule_v6_hdl);
	}
	/* Delete corresponding ipa_rm_resource_name of RX-endpoint after delete all IPV4V6 FT-rule */
	if (rx_prop != NULL)
	{
		IPACMDBG_H("dev %s add producer dependency\n", dev_name);
		IPACMDBG_H("depend Got pipe %d rm index : %d \n", rx_prop->rx[0].src_pipe, IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[rx_prop->rx[0].src_pipe]);
		IPACM_Iface::ipacmcfg->DelRmDepend(IPACM_Iface::ipacmcfg->ipa_client_rm_map_tbl[rx_prop->rx[0].src_pipe]);
		IPACMDBG_H("Finished delete dependency \n ");
		free(rx_prop);
	}

	if (eth_client != NULL)
	{
		free(eth_client);
	}

	if (tx_prop != NULL)
	{
		free(tx_prop);
	}
	if (iface_query != NULL)
	{
		free(iface_query);
	}

	is_active = false;
	post_del_self_evt();

	return res;
}

/* install UL filter rule from Q6 */
int IPACM_Lan::handle_uplink_filter_rule(ipacm_ext_prop *prop, ipa_ip_type iptype, uint8_t xlat_mux_id)
{
	ipa_flt_rule_add flt_rule_entry;
	int len = 0, cnt, ret = IPACM_SUCCESS;
	ipa_ioc_add_flt_rule *pFilteringTable;
	ipa_fltr_installed_notif_req_msg_v01 flt_index;
	int fd;
	int i, index;
	uint32_t value = 0;

	IPACMDBG_H("Set modem UL flt rules\n");

	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	if(prop == NULL || prop->num_ext_props <= 0)
	{
		IPACMDBG_H("No extended property.\n");
		return IPACM_SUCCESS;
	}

	fd = open(IPA_DEVICE_NAME, O_RDWR);
	if (0 == fd)
	{
		IPACMERR("Failed opening %s.\n", IPA_DEVICE_NAME);
		return IPACM_FAILURE;
	}
	if (prop->num_ext_props > MAX_WAN_UL_FILTER_RULES)
	{
		IPACMERR("number of modem UL rules > MAX_WAN_UL_FILTER_RULES, aborting...\n");
		close(fd);
		return IPACM_FAILURE;
	}

	memset(&flt_index, 0, sizeof(flt_index));
	flt_index.source_pipe_index = ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, rx_prop->rx[0].src_pipe);
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	if (tx_prop && IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
	{
		flt_index.dst_pipe_id_valid = 1;
		flt_index.dst_pipe_id_len = tx_prop->num_tx_props;
		for (i = 0; i < tx_prop->num_tx_props && i < QMI_IPA_MAX_CLIENT_DST_PIPES; i++)
		{
			flt_index.dst_pipe_id[i] = ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, tx_prop->tx[i].dst_pipe);
		}
	}
#endif
	flt_index.install_status = IPA_QMI_RESULT_SUCCESS_V01;
#ifndef FEATURE_IPA_V3
	flt_index.filter_index_list_len = prop->num_ext_props;
#else /* defined (FEATURE_IPA_V3) */
	flt_index.rule_id_valid = 1;
	flt_index.rule_id_len = prop->num_ext_props;
#endif
	flt_index.embedded_pipe_index_valid = 1;
	flt_index.embedded_pipe_index = ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, IPA_CLIENT_APPS_LAN_WAN_PROD);
	flt_index.retain_header_valid = 1;
	flt_index.retain_header = 0;
	flt_index.embedded_call_mux_id_valid = 1;
	flt_index.embedded_call_mux_id = IPACM_Iface::ipacmcfg->GetQmapId();
#ifndef FEATURE_IPA_V3
	IPACMDBG_H("flt_index: src pipe: %d, num of rules: %d, ebd pipe: %d, mux id: %d\n",
		flt_index.source_pipe_index, flt_index.filter_index_list_len, flt_index.embedded_pipe_index, flt_index.embedded_call_mux_id);
#else /* defined (FEATURE_IPA_V3) */
	IPACMDBG_H("flt_index: src pipe: %d, num of rules: %d, ebd pipe: %d, mux id: %d\n",
		flt_index.source_pipe_index, flt_index.rule_id_len, flt_index.embedded_pipe_index, flt_index.embedded_call_mux_id);
#endif
	len = sizeof(struct ipa_ioc_add_flt_rule) + prop->num_ext_props * sizeof(struct ipa_flt_rule_add);
	pFilteringTable = (struct ipa_ioc_add_flt_rule*)malloc(len);
	if (pFilteringTable == NULL)
	{
		IPACMERR("Error Locate ipa_flt_rule_add memory...\n");
		close(fd);
		return IPACM_FAILURE;
	}
	memset(pFilteringTable, 0, len);

	pFilteringTable->commit = 1;
	pFilteringTable->ep = rx_prop->rx[0].src_pipe;
	pFilteringTable->global = false;
	pFilteringTable->ip = iptype;
	pFilteringTable->num_rules = prop->num_ext_props;

	memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add)); // Zero All Fields
	flt_rule_entry.at_rear = 1;
#ifdef FEATURE_IPA_V3
	if (flt_rule_entry.rule.eq_attrib.ipv4_frag_eq_present)
		flt_rule_entry.at_rear = 0;
#endif
	flt_rule_entry.flt_rule_hdl = -1;
	flt_rule_entry.status = -1;

	flt_rule_entry.rule.retain_hdr = 0;
	flt_rule_entry.rule.to_uc = 0;
	flt_rule_entry.rule.eq_attrib_type = 1;
	if(iptype == IPA_IP_v4)
	{
		if (ipa_if_cate == ODU_IF && IPACM_Wan::isWan_Bridge_Mode())
		{
			IPACMDBG_H("WAN, ODU are in bridge mode \n");
			flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
		}
		else
		{
			flt_rule_entry.rule.action = IPA_PASS_TO_SRC_NAT;
		}
	}
	else if(iptype == IPA_IP_v6)
		flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
	else
	{
		IPACMERR("IP type is not expected.\n");
		ret = IPACM_FAILURE;
		goto fail;
	}

	index = IPACM_Iface::ipacmcfg->getFltRuleCount(rx_prop->rx[0].src_pipe, iptype);

	for(cnt=0; cnt<prop->num_ext_props; cnt++)
	{
		memcpy(&flt_rule_entry.rule.eq_attrib,
					 &prop->prop[cnt].eq_attrib,
					 sizeof(prop->prop[cnt].eq_attrib));
		flt_rule_entry.rule.rt_tbl_idx = prop->prop[cnt].rt_tbl_idx;

		/* Handle XLAT configuration */
		if ((iptype == IPA_IP_v4) && prop->prop[cnt].is_xlat_rule && (xlat_mux_id != 0))
		{
			/* fill the value of meta-data */
			value = xlat_mux_id;
			flt_rule_entry.rule.eq_attrib.metadata_meq32_present = 1;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.offset = 0;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.value = (value & 0xFF) << 16;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.mask = 0x00FF0000;
			IPACMDBG_H("xlat meta-data is modified for rule: %d has index %d with xlat_mux_id: %d\n",
					cnt, index, xlat_mux_id);
		}
#ifdef FEATURE_IPA_V3
		flt_rule_entry.rule.hashable = prop->prop[cnt].is_rule_hashable;
		flt_rule_entry.rule.rule_id = prop->prop[cnt].rule_id;
		if(rx_prop->rx[0].attrib.attrib_mask & IPA_FLT_META_DATA)	//turn on meta-data equation
		{
			flt_rule_entry.rule.eq_attrib.rule_eq_bitmap |= (1<<9);
			flt_rule_entry.rule.eq_attrib.metadata_meq32_present = 1;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.offset = 0;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.value |= rx_prop->rx[0].attrib.meta_data;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.mask |= rx_prop->rx[0].attrib.meta_data_mask;
		}
#endif
		memcpy(&pFilteringTable->rules[cnt], &flt_rule_entry, sizeof(flt_rule_entry));

		IPACMDBG_H("Modem UL filtering rule %d has index %d\n", cnt, index);
#ifndef FEATURE_IPA_V3
		flt_index.filter_index_list[cnt].filter_index = index;
		flt_index.filter_index_list[cnt].filter_handle = prop->prop[cnt].filter_hdl;
#else /* defined (FEATURE_IPA_V3) */
		flt_index.rule_id[cnt] = prop->prop[cnt].rule_id;
#endif
		index++;
	}

	if(false == m_filtering.SendFilteringRuleIndex(&flt_index))
	{
		IPACMERR("Error sending filtering rule index, aborting...\n");
		ret = IPACM_FAILURE;
		goto fail;
	}

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	if (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable == false)
#endif
	{
		if(false == m_filtering.AddFilteringRule(pFilteringTable))
		{
			IPACMERR("Error Adding RuleTable to Filtering, aborting...\n");
			ret = IPACM_FAILURE;
			goto fail;
		}
		else
		{
			if(iptype == IPA_IP_v4)
			{
				for(i=0; i<pFilteringTable->num_rules; i++)
				{
					wan_ul_fl_rule_hdl_v4[num_wan_ul_fl_rule_v4] = pFilteringTable->rules[i].flt_rule_hdl;
					num_wan_ul_fl_rule_v4++;
				}
				IPACM_Iface::ipacmcfg->increaseFltRuleCount(rx_prop->rx[0].src_pipe, iptype, pFilteringTable->num_rules);
			}
			else if(iptype == IPA_IP_v6)
			{
				for(i=0; i<pFilteringTable->num_rules; i++)
				{
					wan_ul_fl_rule_hdl_v6[num_wan_ul_fl_rule_v6] = pFilteringTable->rules[i].flt_rule_hdl;
					num_wan_ul_fl_rule_v6++;
				}
				IPACM_Iface::ipacmcfg->increaseFltRuleCount(rx_prop->rx[0].src_pipe, iptype, pFilteringTable->num_rules);
			}
			else
			{
				IPACMERR("IP type is not expected.\n");
				goto fail;
			}
		}
	}
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	else
	{
		if(iptype == IPA_IP_v4)
		{
			num_wan_ul_fl_rule_v4 = pFilteringTable->num_rules;
		}
		else if(iptype == IPA_IP_v6)
		{
			num_wan_ul_fl_rule_v6 = pFilteringTable->num_rules;
		}
		else
		{
			IPACMERR("IP type is not expected.\n");
			goto fail;
		}

	}
#endif
fail:
	free(pFilteringTable);
	close(fd);
	return ret;
}

#ifdef FEATURE_IPACM_UL_FIREWALL
void IPACM_Lan::change_to_network_order(ipa_ip_type iptype, ipa_rule_attrib* attrib)
{
	if(attrib == NULL)
	{
		IPACMERR("Attribute pointer is NULL.\n");
		return;
	}

	if(iptype == IPA_IP_v6)
	{
		int i;
		for(i=0; i<4; i++)
		{
			attrib->u.v6.src_addr[i] = htonl(attrib->u.v6.src_addr[i]);
			attrib->u.v6.src_addr_mask[i] = htonl(attrib->u.v6.src_addr_mask[i]);
			attrib->u.v6.dst_addr[i] = htonl(attrib->u.v6.dst_addr[i]);
			attrib->u.v6.dst_addr_mask[i] = htonl(attrib->u.v6.dst_addr_mask[i]);
		}
	}
	else
	{
		IPACMDBG_H("IP type is not IPv6, do nothing: %d\n", iptype);
	}

	return;
}

/* clean UL firewall filter rules (IPv6 only) */
int IPACM_Lan::delete_uplink_filter_rule_ul(ipa_ip_type iptype, ul_firewall_t *ul_firewall)
{
	uint32_t *flt_rule_hdls = NULL;
	int num_of_rules = 0;
	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	if (iptype != IPA_IP_v6)
	{
		IPACMDBG_H("Invalid IP type: Not Applicable\n");
		return IPACM_SUCCESS;
	}

	memset(IPACM_Wan::firewall_flt_rule_v6_ul,
		0, (IPACM_MAX_FIREWALL_ENTRIES + 1) * sizeof(ipa_flt_rule_add));

	if (false == ul_firewall->ul_firewall_installed)
	{
		IPACMDBG_H ("No UL firewall installed\n");
		return IPACM_SUCCESS;
	}

	IPACMDBG_H("Deleting UL firewall rules for pipe (%d)\n", rx_prop->rx[0].src_pipe);

	if (true == ul_firewall->ul_frag_installed)
	{
		flt_rule_hdls = &ul_firewall->ul_frag_handle;

		if (m_filtering.DeleteFilteringHdls(flt_rule_hdls, IPA_IP_v6, 1) == false)
		{
			IPACMERR("Error deleting IPv6 UL frag filtering rules.\n");
			return IPACM_FAILURE;
		}
		ul_firewall->ul_frag_installed = false;
		IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, 1);
		IPACMDBG_H("Frag deleted successfully\n");
	}

	num_of_rules = IPACM_Wan::num_firewall_v6_ul;

	if (true == ul_firewall->ul_catch_installed)
		num_of_rules++;

	if (num_of_rules)
	{
		flt_rule_hdls = ul_firewall->ul_firewall_handle;
		if (m_filtering.DeleteFilteringHdls(flt_rule_hdls,
					IPA_IP_v6, num_of_rules) == false)
		{
			IPACMERR("Error Deleting UL Filtering rules, aborting...\n");
			return IPACM_FAILURE;
		}
		IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, num_of_rules);
		IPACMDBG_H("%d num UL rules on pipe (%d) deleted successfully\n", num_of_rules, rx_prop->rx[0].src_pipe);
	}
	else
	{
		IPACMDBG_H("No UL Firewall filter rule to delete\n");
	}
	memset(ul_firewall, 0, sizeof (ul_firewall_t));
	return IPACM_SUCCESS;
}

/* Send UL firewall WhiteListing rules to Q6 */
int IPACM_Lan::install_wan_firewall_rule_ul(bool enable, ipa_ip_type iptype)
{
	int len, res = IPACM_SUCCESS;
	uint8_t mux_id, num_of_ul_rules = IPACM_Wan::num_firewall_v6_ul;
	ipa_ioc_add_flt_rule *pFilteringTable_v6 = NULL;
	mux_id = IPACM_Iface::ipacmcfg->GetQmapId();
	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	if (iptype != IPA_IP_v6)
	{
		IPACMDBG_H("Invalid IP type: Not Applicable\n");
		return IPACM_SUCCESS;
	}

	if (false == enable)
	{
		num_of_ul_rules = 0;
		if (false == IPACM_Wan::is_v6_ul_firewall_sent_to_q6)
		{
			IPACMDBG_H ("UL filter rules on Q6 is already disabled\n");
			return IPACM_SUCCESS;
		}
	}

	/* Not considering is_sw_routing and embm is on or off */

	if (num_of_ul_rules >= 0)
	{
		len = sizeof(struct ipa_ioc_add_flt_rule) + num_of_ul_rules * sizeof(struct ipa_flt_rule_add);
		pFilteringTable_v6 = (struct ipa_ioc_add_flt_rule*)malloc(len);

		IPACMDBG_H("Total number of WAN UL filtering rule for IPv6 is %d : mux_id (%d)\n", num_of_ul_rules,
			mux_id);

		if (pFilteringTable_v6 == NULL)
		{
			IPACMERR("Error Locate ipa_flt_rule_add memory...\n");
			return IPACM_FAILURE;
		}
		memset(pFilteringTable_v6, 0, len);
		pFilteringTable_v6->commit = 1;
		pFilteringTable_v6->ep = rx_prop->rx[0].src_pipe;
		pFilteringTable_v6->global = false;
		pFilteringTable_v6->ip = IPA_IP_v6;
		pFilteringTable_v6->num_rules = (uint8_t)num_of_ul_rules;

		memcpy(pFilteringTable_v6->rules, IPACM_Wan::firewall_flt_rule_v6_ul, num_of_ul_rules * sizeof(ipa_flt_rule_add));
	}
	if (false == m_filtering.AddWanULFilteringRule(pFilteringTable_v6, mux_id, enable))
	{
		IPACMERR("Failed to install WAN UL filtering table.\n");
		res = IPACM_FAILURE;
		goto fail;
	}

	IPACM_Wan::is_v6_ul_firewall_sent_to_q6 = enable;

fail:
	if (pFilteringTable_v6 != NULL)
	{
		free (pFilteringTable_v6);
	}
	return res;
}

/* Config UL frag firewall filter rules */
int IPACM_Lan::config_wan_frag_firewall_rule_ul_ex(bool install, ipa_ip_type iptype, ul_firewall_t *ul_firewall)
{
	struct ipa_flt_rule_add flt_rule_entry;
	int len = 0, index, rule_v6_ul = 0, iface_type = 0;
	IPACMDBG_H("ip-family: %d; \n", iptype);
	uint32_t *flt_rule_hdls = NULL;

	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	if (iptype != IPA_IP_v6)
	{
		IPACMDBG_H("Invalid IP type: Not Applicable\n");
		return IPACM_SUCCESS;
	}


	if (false == install)
	{
		if (true == ul_firewall->ul_frag_installed)
		{
			flt_rule_hdls = &ul_firewall->ul_frag_handle;

			if (m_filtering.DeleteFilteringHdls(flt_rule_hdls, IPA_IP_v6, 1) == false)
			{
				IPACMERR("Error deleting IPv6 UL frag filtering rules.\n");
				return IPACM_FAILURE;
			}
			ul_firewall->ul_frag_installed = false;
			IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, 1);
			IPACMDBG_H("Frag deleted successfully on pipe (%d)\n", rx_prop->rx[0].src_pipe);
		}
		else
		{
			IPACMDBG_H("No frag rule configurerd on pipe (%d)\n", rx_prop->rx[0].src_pipe);
		}
		return IPACM_SUCCESS;
	}

	/* Frag rule installation */
	/* construct ipa_ioc_add_flt_rule with 1 frag rule */
	ipa_ioc_add_flt_rule *m_pFilteringTable = NULL;
	len = sizeof(struct ipa_ioc_add_flt_rule) + 1 * sizeof(struct ipa_flt_rule_add);
	m_pFilteringTable = (struct ipa_ioc_add_flt_rule *)calloc(1, len);

	if (!m_pFilteringTable)
	{
		IPACMERR("Error Locate ipa_flt_rule_add memory...\n");
		return IPACM_FAILURE;
	}

	ul_firewall->ul_firewall_installed = true;
	ul_firewall->ul_frag_installed = true;

	memset(m_pFilteringTable, 0, len);

	m_pFilteringTable->commit = 1;
	m_pFilteringTable->ep = rx_prop->rx[0].src_pipe;
	m_pFilteringTable->global = false;
	m_pFilteringTable->ip = IPA_IP_v6;
	m_pFilteringTable->num_rules = (uint8_t)1;

	memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));
	flt_rule_entry.at_rear = false;
	flt_rule_entry.rule.hashable = false;
	flt_rule_entry.flt_rule_hdl = -1;
	flt_rule_entry.status = -1;
	flt_rule_entry.rule.action = IPA_PASS_TO_EXCEPTION;
	flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_FRAGMENT;

	memcpy(&(m_pFilteringTable->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));

	if (false == m_filtering.AddFilteringRule(m_pFilteringTable))
	{
		IPACMERR("Error Adding RuleTable(0) to Filtering, aborting...\n");
		free(m_pFilteringTable);
		return IPACM_FAILURE;
	}
	else
	{
		IPACM_Iface::ipacmcfg->increaseFltRuleCount(m_pFilteringTable->ep, IPA_IP_v6, 1);
		IPACMDBG_H("flt rule hdl0=0x%x, status=0x%x\n", m_pFilteringTable->rules[0].flt_rule_hdl, m_pFilteringTable->rules[0].status);
		ul_firewall->ul_frag_handle = m_pFilteringTable->rules[0].flt_rule_hdl;
	}
	return IPACM_SUCCESS;
}

/* Configure UL firewall rules, to be sent to Q6 side*/
int IPACM_Lan::config_dft_firewall_rules_ul_ex(struct ipa_flt_rule_add *rules, ipa_ip_type iptype)
{
	struct ipa_flt_rule_add flt_rule_entry;
	ipa_ioc_get_rt_tbl_indx rt_tbl_idx;
	ipa_ioc_generate_flt_eq flt_eq;
	int i, len = 0, rule_v6_ul = 0;

	IPACMDBG_H("ip-family: %d; \n", iptype);

	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	if (iptype != IPA_IP_v6)
	{
		IPACMDBG_H("Invalid IP type: Not Applicable\n");
		return IPACM_SUCCESS;
	}

	if (IPACM_Wan::firewall_config_ul.firewall_enable == true)
	{
		for (i = 0; i < IPACM_Wan::firewall_config_ul.num_extd_firewall_entries; i++)
		{
			if (IPACM_Wan::firewall_config_ul.extd_firewall_entries[i].ip_vsn == 6 &&
				IPACM_Wan::firewall_config_ul.extd_firewall_entries[i].firewall_direction
				== IPACM_MSGR_UL_FIREWALL)
			{
				memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));
				flt_rule_entry.at_rear = true;
				flt_rule_entry.flt_rule_hdl = -1;
				flt_rule_entry.status = -1;
				flt_rule_entry.rule.retain_hdr = 1;
				flt_rule_entry.rule.to_uc = 0;
				flt_rule_entry.rule.eq_attrib_type = 1;
				flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;

				flt_rule_entry.rule.hashable = true;

				memset(&rt_tbl_idx, 0, sizeof(rt_tbl_idx));
				rt_tbl_idx.ip = iptype;
				/* matched rules for v6 go PASS_TO_ROUTE */
				if(IPACM_Wan::firewall_config_ul.rule_action_accept == true)
				{
					strlcpy(rt_tbl_idx.name, IPACM_Iface::ipacmcfg->rt_tbl_wan_v6.name, IPA_RESOURCE_NAME_MAX);
				}
				else
				{
					strlcpy(rt_tbl_idx.name, IPACM_Iface::ipacmcfg->rt_tbl_wan_dl.name, IPA_RESOURCE_NAME_MAX);
				}
				rt_tbl_idx.name[IPA_RESOURCE_NAME_MAX-1] = '\0';
				if(0 != ioctl(IPACM_Wan::m_fd_ipa_ul, IPA_IOC_QUERY_RT_TBL_INDEX, &rt_tbl_idx))
				{
					IPACMERR("Failed to get routing table index from name\n");
					return IPACM_FAILURE;
				}
				flt_rule_entry.rule.rt_tbl_idx = rt_tbl_idx.idx;
				IPACMDBG_H("Routing table %s has index %d\n", rt_tbl_idx.name, rt_tbl_idx.idx);

				memcpy(&flt_rule_entry.rule.attrib,
				&IPACM_Wan::firewall_config_ul.extd_firewall_entries[i].attrib,
				sizeof(struct ipa_rule_attrib));
				flt_rule_entry.rule.attrib.attrib_mask |= rx_prop->rx[0].attrib.attrib_mask;
				flt_rule_entry.rule.attrib.attrib_mask &= ~IPA_FLT_META_DATA;
				flt_rule_entry.rule.attrib.meta_data_mask = rx_prop->rx[0].attrib.meta_data_mask;
				flt_rule_entry.rule.attrib.meta_data = rx_prop->rx[0].attrib.meta_data;
				change_to_network_order(IPA_IP_v6, &flt_rule_entry.rule.attrib);

				/* check if the rule is define as TCP/UDP */
				if (IPACM_Wan::firewall_config_ul.extd_firewall_entries[i].attrib.u.v6.next_hdr == IPACM_FIREWALL_IPPROTO_TCP_UDP)
				{
					/* insert TCP rule*/
					flt_rule_entry.rule.attrib.u.v6.next_hdr = IPACM_FIREWALL_IPPROTO_TCP;
					memset(&flt_eq, 0, sizeof(flt_eq));
					memcpy(&flt_eq.attrib, &flt_rule_entry.rule.attrib, sizeof(flt_eq.attrib));
					flt_eq.ip = iptype;
					if(0 != ioctl(IPACM_Wan::m_fd_ipa_ul, IPA_IOC_GENERATE_FLT_EQ, &flt_eq))
					{
						IPACMERR("Failed to get eq_attrib\n");
						return IPACM_FAILURE;
					}

					memcpy(&flt_rule_entry.rule.eq_attrib,
						&flt_eq.eq_attrib,
						sizeof(flt_rule_entry.rule.eq_attrib));
					memcpy(&(rules[rule_v6_ul]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));
					rule_v6_ul++;

					/* insert UDP rule*/
					flt_rule_entry.rule.attrib.u.v6.next_hdr = IPACM_FIREWALL_IPPROTO_UDP;
					memset(&flt_eq, 0, sizeof(flt_eq));
					memcpy(&flt_eq.attrib, &flt_rule_entry.rule.attrib, sizeof(flt_eq.attrib));
					flt_eq.ip = iptype;
					if(0 != ioctl(IPACM_Wan::m_fd_ipa_ul, IPA_IOC_GENERATE_FLT_EQ, &flt_eq))
					{
						IPACMERR("Failed to get eq_attrib\n");
						return IPACM_FAILURE;
					}

					memcpy(&flt_rule_entry.rule.eq_attrib,
						&flt_eq.eq_attrib,
						sizeof(flt_rule_entry.rule.eq_attrib));
					memcpy(&(rules[rule_v6_ul]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));
					rule_v6_ul++;
				}
				else
				{
					memset(&flt_eq, 0, sizeof(flt_eq));
					memcpy(&flt_eq.attrib, &flt_rule_entry.rule.attrib, sizeof(flt_eq.attrib));
					flt_eq.ip = iptype;
					if(0 != ioctl(IPACM_Wan::m_fd_ipa_ul, IPA_IOC_GENERATE_FLT_EQ, &flt_eq))
					{
						IPACMERR("Failed to get eq_attrib\n");
						return IPACM_FAILURE;
					}

					memcpy(&flt_rule_entry.rule.eq_attrib,
						&flt_eq.eq_attrib,
						sizeof(flt_rule_entry.rule.eq_attrib));
					memcpy(&(rules[rule_v6_ul]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));
					rule_v6_ul++;
				}
			}
		} /* end of firewall ipv6 filter rule add for loop*/
		IPACM_Lan::install_wan_firewall_rule_ul(true, iptype);
	}
	IPACMDBG_H ("Configured (%d == %d) UL firewall rules to be sent to Q6\n ", IPACM_Wan::num_firewall_v6_ul, rule_v6_ul);
	IPACMDBG_H ("Firewall Status (%d)\n", IPACM_Wan::firewall_config_ul.firewall_enable);
	return IPACM_SUCCESS;
}

int IPACM_Lan::re_config_dft_firewall_rules_ul(ipa_ip_type iptype, ul_firewall_t *ul_firewall)
{
	int index, iface_type;

	if (iptype != IPA_IP_v6)
	{
		IPACMDBG_H("Invalid IP type: Not Applicable\n");
		return IPACM_SUCCESS;
	}

	IPACM_Wan::read_firewall_filter_rules_ul();
	IPACMDBG_H ("Firewall Status (%d)\n", IPACM_Wan::firewall_config_ul.firewall_enable);
	IPACMDBG_H("UL firewall needs to be reconfig on pipe (%d)\n", rx_prop->rx[0].src_pipe);
	if (IPACM_Wan::firewall_config_ul.firewall_enable )
	{
		if (IPACM_Wan::isWanUP_V6(ipa_if_num) &&
				IPACM_Wan::firewall_config_ul.rule_action_accept == true) /* check BH and WL/BL?*/
		{
			delete_uplink_filter_rule_ul(IPA_IP_v6, ul_firewall);

			/* Configure and send the firewall filter table to Q6*/
			IPACM_Lan::config_dft_firewall_rules_ul_ex(IPACM_Wan::firewall_flt_rule_v6_ul, IPA_IP_v6);
			IPACM_Lan::config_wan_frag_firewall_rule_ul_ex(true, IPA_IP_v6, ul_firewall);
			IPACMDBG_H ("New config rules sent to Q6\n");
		}
		else
		{
			IPACMDBG_H ("Send indication to Q6 to disable UL firewall\n");
			IPACM_Lan::config_wan_frag_firewall_rule_ul_ex(false, IPA_IP_v6, ul_firewall);
			install_wan_firewall_rule_ul(false, IPA_IP_v6);

			IPACM_Lan::config_dft_firewall_rules_ul(IPACM_Wan::firewall_flt_rule_v6_ul, IPA_IP_v6, ul_firewall);
			IPACMDBG_H ("Pipe (%d) reconfigured with the new UL rules\n", rx_prop->rx[0].src_pipe);
		}
	}
	else if (ul_firewall->ul_firewall_installed &&
			!IPACM_Wan::firewall_config_ul.firewall_enable)
	{
		delete_uplink_filter_rule_ul(IPA_IP_v6, ul_firewall);
	}
	else
	{
		IPACMDBG_H ("UL firewall rules are not installed \n");
	}

	if (IPACM_Wan::is_v6_ul_firewall_sent_to_q6 == true &&
		IPACM_Wan::firewall_config_ul.firewall_enable == false)
	{
		IPACMDBG_H ("Send indication to Q6 to disable UL firewall\n");
		IPACM_Lan::config_wan_frag_firewall_rule_ul_ex(false, IPA_IP_v6, ul_firewall);
		install_wan_firewall_rule_ul(false, IPA_IP_v6);
	}
	else if (IPACM_Wan::is_v6_ul_firewall_sent_to_q6 == false)
	{
		IPACMDBG_H ("UL firewall rules are not installed on Q6 \n");
	}

	return IPACM_SUCCESS;
}

/* Configure and install UL firewall rules, to be installed on client side */
int IPACM_Lan::config_dft_firewall_rules_ul(struct ipa_flt_rule_add *rules, ipa_ip_type iptype,
				ul_firewall_t *ul_firewall)
{
	struct ipa_flt_rule_add flt_rule_entry;
	int len = 0, i, rule_v6_ul = 0;
	IPACMDBG_H("ip-family: %d; \n", iptype);

	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	if (iptype != IPA_IP_v6)
	{
		IPACMDBG_H("Invalid IP type: Not Applicable\n");
		return IPACM_SUCCESS;
	}

	/* Delete the existing UL rules */
	delete_uplink_filter_rule_ul(iptype, ul_firewall);

	/* construct ipa_ioc_add_flt_rule with N firewall rules */
	ipa_ioc_add_flt_rule *m_pFilteringTable = NULL;
	len = sizeof(struct ipa_ioc_add_flt_rule) + 1 * sizeof(struct ipa_flt_rule_add);
	m_pFilteringTable = (struct ipa_ioc_add_flt_rule *)calloc(1, len);

	if (!m_pFilteringTable)
	{
		IPACMERR("Error Locate ipa_flt_rule_add memory...\n");
		return IPACM_FAILURE;
	}

	if (false == m_routing.GetRoutingTable(&IPACM_Iface::ipacmcfg->rt_tbl_wan_v6))
	{
		IPACMERR("m_routing.GetRoutingTable(rt_tbl_wan_v6) Failed.\n");
		free(m_pFilteringTable);
		return IPACM_FAILURE;
	}

	/* Catch-all filter rule in case of whitelisting case, redirecting packets to execption path */
	if (IPACM_Wan::firewall_config_ul.firewall_enable == true &&
		IPACM_Wan::firewall_config_ul.rule_action_accept == true)
	{
		ul_firewall->ul_firewall_installed = true;
		ul_firewall->ul_catch_installed = true;
		memset(m_pFilteringTable, 0, len);
		m_pFilteringTable->commit = 1;

		m_pFilteringTable->ep = rx_prop->rx[0].src_pipe;
		m_pFilteringTable->global = false;
		m_pFilteringTable->ip = IPA_IP_v6;
		m_pFilteringTable->num_rules = (uint8_t)1;

		IPACMDBG_H ("Catch all rule to drop all in excep path\n");

		memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));
	   	flt_rule_entry.at_rear = false;
		flt_rule_entry.flt_rule_hdl = -1;
		flt_rule_entry.status = -1;
		flt_rule_entry.rule.action = IPA_PASS_TO_EXCEPTION;
		flt_rule_entry.rule.hashable = true;
		memcpy(&(m_pFilteringTable->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));

		if (false == m_filtering.AddFilteringRule(m_pFilteringTable))
		{
			IPACMERR("Error Adding Filtering rules, aborting...\n");
			free(m_pFilteringTable);
			return IPACM_FAILURE;
		}
		else
		{
			IPACM_Iface::ipacmcfg->increaseFltRuleCount(m_pFilteringTable->ep, IPA_IP_v6, 1);
			/* save v6 firewall filter rule handler */
			IPACMDBG_H("flt rule hdl0=0x%x, status=0x%x\n", m_pFilteringTable->rules[0].flt_rule_hdl, m_pFilteringTable->rules[0].status);
			ul_firewall->ul_firewall_handle[rule_v6_ul++] = m_pFilteringTable->rules[0].flt_rule_hdl;
		}
	}

	if (IPACM_Wan::firewall_config_ul.firewall_enable == true &&
		(IPACM_Wan::check_dft_firewall_rules_attr_mask_ul(&IPACM_Wan::firewall_config_ul) ||
		IPACM_Wan::firewall_config_ul.rule_action_accept == true))
	{
		ul_firewall->ul_firewall_installed = true;
		ul_firewall->ul_frag_installed = true;
		memset(m_pFilteringTable, 0, len);

		m_pFilteringTable->commit = 1;
		m_pFilteringTable->ep = rx_prop->rx[0].src_pipe;

		memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));

		if (IPACM_Wan::firewall_config_ul.rule_action_accept != true)
			memcpy(&flt_rule_entry.rule.attrib, &rx_prop->rx[0].attrib, sizeof(struct ipa_rule_attrib));

		m_pFilteringTable->global = false;
		m_pFilteringTable->ip = IPA_IP_v6;
		m_pFilteringTable->num_rules = (uint8_t)1;

		flt_rule_entry.at_rear = false;

		flt_rule_entry.rule.hashable = false;

		flt_rule_entry.flt_rule_hdl = -1;
		flt_rule_entry.status = -1;
		flt_rule_entry.rule.action = IPA_PASS_TO_EXCEPTION;

		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_FRAGMENT;
		memcpy(&(m_pFilteringTable->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));
		if (false == m_filtering.AddFilteringRule(m_pFilteringTable))
		{
			IPACMERR("Error Adding RuleTable(0) to Filtering, aborting...\n");
			free(m_pFilteringTable);
			return IPACM_FAILURE;
		}
		else
		{
			IPACM_Iface::ipacmcfg->increaseFltRuleCount(m_pFilteringTable->ep, IPA_IP_v6, 1);
			IPACMDBG_H("flt rule hdl0=0x%x, status=0x%x\n", m_pFilteringTable->rules[0].flt_rule_hdl, m_pFilteringTable->rules[0].status);
			ul_firewall->ul_frag_handle = m_pFilteringTable->rules[0].flt_rule_hdl;
		}
	}

	memset(m_pFilteringTable, 0, len);
	m_pFilteringTable->commit = 1;
	m_pFilteringTable->ep = rx_prop->rx[0].src_pipe;
	m_pFilteringTable->global = false;
	m_pFilteringTable->ip = IPA_IP_v6;
	m_pFilteringTable->num_rules = (uint8_t)1;

	if (IPACM_Wan::firewall_config_ul.firewall_enable == true)
	{
		IPACMDBG_H("Firewall enabled\n");

		for (i = 0; i < IPACM_Wan::firewall_config_ul.num_extd_firewall_entries; i++)
		{
			if (IPACM_Wan::firewall_config_ul.extd_firewall_entries[i].ip_vsn == 6 &&
				IPACM_Wan::firewall_config_ul.extd_firewall_entries[i].firewall_direction ==
				IPACM_MSGR_UL_FIREWALL)
			{
				ul_firewall->ul_firewall_installed = true;
				memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));
				flt_rule_entry.at_rear = false;
				flt_rule_entry.flt_rule_hdl = -1;
				flt_rule_entry.status = -1;

			    /* matched rules for v6 go PASS_TO_ROUTE */
				if(IPACM_Wan::firewall_config_ul.rule_action_accept == true)
				{
					flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
				}
				else
				{
					flt_rule_entry.rule.action = IPA_PASS_TO_EXCEPTION;
				}

				flt_rule_entry.rule.hashable = true;

				flt_rule_entry.rule.rt_tbl_hdl = IPACM_Iface::ipacmcfg->rt_tbl_wan_v6.hdl;
				memcpy(&flt_rule_entry.rule.attrib,
				&IPACM_Wan::firewall_config_ul.extd_firewall_entries[i].attrib,
				sizeof(struct ipa_rule_attrib));

				flt_rule_entry.rule.attrib.attrib_mask |= rx_prop->rx[0].attrib.attrib_mask;
				flt_rule_entry.rule.attrib.meta_data_mask = rx_prop->rx[0].attrib.meta_data_mask;
				flt_rule_entry.rule.attrib.meta_data = rx_prop->rx[0].attrib.meta_data;

				/* check if the rule is define as TCP/UDP */
				if (IPACM_Wan::firewall_config_ul.extd_firewall_entries[i].attrib.u.v6.next_hdr == IPACM_FIREWALL_IPPROTO_TCP_UDP)
				{
					/* insert TCP rule*/
					flt_rule_entry.rule.attrib.u.v6.next_hdr = IPACM_FIREWALL_IPPROTO_TCP;
					memcpy(&(m_pFilteringTable->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));
					if (false == m_filtering.AddFilteringRule(m_pFilteringTable))
					{
						IPACMERR("Error Adding Filtering rules, aborting...\n");
						free(m_pFilteringTable);
						return IPACM_FAILURE;
					}
					else
					{
						IPACM_Iface::ipacmcfg->increaseFltRuleCount(m_pFilteringTable->ep, IPA_IP_v6, 1);
						/* save v4 firewall filter rule handler */
						IPACMDBG_H("flt rule hdl0=0x%x, status=0x%x\n", m_pFilteringTable->rules[0].flt_rule_hdl, m_pFilteringTable->rules[0].status);
						ul_firewall->ul_firewall_handle[rule_v6_ul++] = m_pFilteringTable->rules[0].flt_rule_hdl;
					}

					/* insert UDP rule*/
					flt_rule_entry.rule.attrib.u.v6.next_hdr = IPACM_FIREWALL_IPPROTO_UDP;
					memcpy(&(m_pFilteringTable->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));
					if (false == m_filtering.AddFilteringRule(m_pFilteringTable))
					{
						IPACMERR("Error Adding Filtering rules, aborting...\n");
						free(m_pFilteringTable);
						return IPACM_FAILURE;
					}
					else
					{
						IPACM_Iface::ipacmcfg->increaseFltRuleCount(m_pFilteringTable->ep, IPA_IP_v6, 1);
						/* save v6 firewall filter rule handler */
						IPACMDBG_H("flt rule hdl0=0x%x, status=0x%x\n", m_pFilteringTable->rules[0].flt_rule_hdl, m_pFilteringTable->rules[0].status);
						ul_firewall->ul_firewall_handle[rule_v6_ul++] = m_pFilteringTable->rules[0].flt_rule_hdl;
					}
				}
				else
				{
					memcpy(&(m_pFilteringTable->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));
					if (false == m_filtering.AddFilteringRule(m_pFilteringTable))
					{
						IPACMERR("Error Adding Filtering rules, aborting...\n");
						free(m_pFilteringTable);
						return IPACM_FAILURE;
					}
					else
					{
						IPACM_Iface::ipacmcfg->increaseFltRuleCount(m_pFilteringTable->ep, IPA_IP_v6, 1);
						/* save v6 firewall filter rule handler */
						IPACMDBG_H("flt rule hdl0=0x%x, status=0x%x\n", m_pFilteringTable->rules[0].flt_rule_hdl, m_pFilteringTable->rules[0].status);
						ul_firewall->ul_firewall_handle[rule_v6_ul++] = m_pFilteringTable->rules[0].flt_rule_hdl;
					}
				}
			}
		} /* end of firewall ipv6 filter rule add for loop*/
	}
	IPACMDBG_H ("Configured and installed (%d == %d) UL firewall rules on pipe (%d)\n ",
		IPACM_Wan::num_firewall_v6_ul, rule_v6_ul, (int)m_pFilteringTable->ep);
	IPACMDBG_H ("Firewall Status (%d)\n", IPACM_Wan::firewall_config_ul.firewall_enable);
	if(m_pFilteringTable != NULL)
	{
		free(m_pFilteringTable);
	}
	return IPACM_SUCCESS;
}
#endif //FEATURE_IPACM_UL_FIREWALL
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
/* install UL filter rule from Q6 per client */
int IPACM_Lan::install_uplink_filter_rule_per_client
(
	ipacm_ext_prop* prop,
	ipa_ip_type iptype,
	uint8_t xlat_mux_id,
	uint8_t *mac_addr
)
{
	ipa_flt_rule_add flt_rule_entry;
	int len = 0, cnt, ret = IPACM_SUCCESS;
	ipa_ioc_add_flt_rule *pFilteringTable;
	int fd;
	int i, index = 0;
	uint32_t value = 0;
	int clnt_indx;
	uint8_t num_offset_meq_128 = 0;
	struct ipa_ipfltr_mask_eq_128 *offset_meq_128 = NULL;

	IPACMDBG_H("Set modem UL flt rules\n");

	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	if(prop == NULL || prop->num_ext_props <= 0)
	{
		IPACMDBG_H("No extended property.\n");
		return IPACM_SUCCESS;
	}

	clnt_indx = get_eth_client_index(mac_addr);

	if (clnt_indx == IPACM_INVALID_INDEX)
	{
		IPACMERR("eth client not found/attached \n");
		return IPACM_FAILURE;
	}

	if (get_client_memptr(eth_client, clnt_indx)->lan_stats_idx == -1)
	{
		IPACMERR("Invalid LAN Stats idx for ethernet client:%d \n", clnt_indx);
		return IPACM_FAILURE;
	}

	fd = open(IPA_DEVICE_NAME, O_RDWR);
	if (0 == fd)
	{
		IPACMERR("Failed opening %s.\n", IPA_DEVICE_NAME);
		return IPACM_FAILURE;
	}
	if (prop->num_ext_props > MAX_WAN_UL_FILTER_RULES)
	{
		IPACMERR("number of modem UL rules > MAX_WAN_UL_FILTER_RULES, aborting...\n");
		close(fd);
		return IPACM_FAILURE;
	}

	len = sizeof(struct ipa_ioc_add_flt_rule) + prop->num_ext_props * sizeof(struct ipa_flt_rule_add);
	pFilteringTable = (struct ipa_ioc_add_flt_rule*)malloc(len);
	if (pFilteringTable == NULL)
	{
		IPACMERR("Error Locate ipa_flt_rule_add memory...\n");
		close(fd);
		return IPACM_FAILURE;
	}
	memset(pFilteringTable, 0, len);

	pFilteringTable->commit = 1;
	pFilteringTable->ep = rx_prop->rx[0].src_pipe;
	pFilteringTable->global = false;
	pFilteringTable->ip = iptype;
	pFilteringTable->num_rules = prop->num_ext_props;

	memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add)); // Zero All Fields
	flt_rule_entry.at_rear = 1;
	if (flt_rule_entry.rule.eq_attrib.ipv4_frag_eq_present)
		flt_rule_entry.at_rear = 0;
	flt_rule_entry.flt_rule_hdl = -1;
	flt_rule_entry.status = -1;

	flt_rule_entry.rule.retain_hdr = 0;
	flt_rule_entry.rule.to_uc = 0;
	flt_rule_entry.rule.eq_attrib_type = 1;
	if(iptype == IPA_IP_v4)
	{
		if (ipa_if_cate == ODU_IF && IPACM_Wan::isWan_Bridge_Mode())
		{
			IPACMDBG_H("WAN, ODU are in bridge mode \n");
			flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
		}
		else
		{
			flt_rule_entry.rule.action = IPA_PASS_TO_SRC_NAT;
		}
	}
	else if(iptype == IPA_IP_v6)
		flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
	else
	{
		IPACMERR("IP type is not expected.\n");
		ret = IPACM_FAILURE;
		goto fail;
	}

	for(cnt=0; cnt<prop->num_ext_props; cnt++)
	{
		memcpy(&flt_rule_entry.rule.eq_attrib,
					 &prop->prop[cnt].eq_attrib,
					 sizeof(prop->prop[cnt].eq_attrib));

		/* Check if we can add the MAC address rule. */
		if (flt_rule_entry.rule.eq_attrib.num_offset_meq_128 == IPA_IPFLTR_NUM_MEQ_128_EQNS)
		{
			IPACMERR("128 bit equations not available.\n");
			ret = IPACM_FAILURE;
			goto fail;
		}
		num_offset_meq_128 = flt_rule_entry.rule.eq_attrib.num_offset_meq_128;
		offset_meq_128 = &flt_rule_entry.rule.eq_attrib.offset_meq_128[num_offset_meq_128];
		if(rx_prop->rx[0].hdr_l2_type == IPA_HDR_L2_ETHERNET_II)
		{
			offset_meq_128->offset = -8;
		}
		else
		{
			offset_meq_128->offset = -16;
		}

		for (i = 0; i < 10; i++)
		{
			offset_meq_128->mask[i] = 0;
			offset_meq_128->value[i] = 0;
		}

		memset(&offset_meq_128->mask[10], 0xFF, ETH_ALEN);

		for ( i = 0; i < ETH_ALEN; i++)
			offset_meq_128->value[10+i] = mac_addr[ETH_ALEN-(i+1)];

		if (num_offset_meq_128 == 0)
			flt_rule_entry.rule.eq_attrib.rule_eq_bitmap |= (1<<3);
		else
			flt_rule_entry.rule.eq_attrib.rule_eq_bitmap |= (1<<4);

		flt_rule_entry.rule.eq_attrib.num_offset_meq_128++;


		flt_rule_entry.rule.rt_tbl_idx = prop->prop[cnt].rt_tbl_idx;

		/* Handle XLAT configuration */
		if ((iptype == IPA_IP_v4) && prop->prop[cnt].is_xlat_rule && (xlat_mux_id != 0))
		{
			/* fill the value of meta-data */
			value = xlat_mux_id;
			flt_rule_entry.rule.eq_attrib.metadata_meq32_present = 1;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.offset = 0;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.value = (value & 0xFF) << 16;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.mask = 0x00FF0000;
			IPACMDBG_H("xlat meta-data is modified for rule: %d has rule_id %d with xlat_mux_id: %d\n",
					cnt, prop->prop[cnt].rule_id, xlat_mux_id);
		}
		IPACMDBG_H("rule: %d has rule_id %d\n",
				cnt, prop->prop[cnt].rule_id);
		flt_rule_entry.rule.hashable = prop->prop[cnt].is_rule_hashable;
		flt_rule_entry.rule.rule_id = (prop->prop[cnt].rule_id & 0x1F) |
			(get_client_memptr(eth_client, clnt_indx)->lan_stats_idx << 5) | 0x200;
		IPACMDBG_H("Modified rule: %d has rule_id %d\n",
				cnt, flt_rule_entry.rule.rule_id);
		if(rx_prop->rx[0].attrib.attrib_mask & IPA_FLT_META_DATA)	//turn on meta-data equation
		{
			flt_rule_entry.rule.eq_attrib.rule_eq_bitmap |= (1<<9);
			flt_rule_entry.rule.eq_attrib.metadata_meq32_present = 1;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.offset = 0;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.value |= rx_prop->rx[0].attrib.meta_data;
			flt_rule_entry.rule.eq_attrib.metadata_meq32.mask |= rx_prop->rx[0].attrib.meta_data_mask;
		}
		memcpy(&pFilteringTable->rules[cnt], &flt_rule_entry, sizeof(flt_rule_entry));

		IPACMDBG_H("Modem UL filtering rule %d has rule_id %d\n", cnt, prop->prop[cnt].rule_id);
		index++;
	}

	if(false == m_filtering.AddFilteringRule(pFilteringTable))
	{
		IPACMERR("Error Adding RuleTable to Filtering, aborting...\n");
		ret = IPACM_FAILURE;
		goto fail;
	}
	else
	{
		if(iptype == IPA_IP_v4)
		{
			for(i=0; i < pFilteringTable->num_rules; i++)
			{
				get_client_memptr(eth_client, clnt_indx)->wan_ul_fl_rule_hdl_v4[i] = pFilteringTable->rules[i].flt_rule_hdl;
			}
			get_client_memptr(eth_client, clnt_indx)->ipv4_ul_rules_set = true;
		}
		else if(iptype == IPA_IP_v6)
		{
			for(i=0; i < pFilteringTable->num_rules; i++)
			{
				get_client_memptr(eth_client, clnt_indx)->wan_ul_fl_rule_hdl_v6[i] = pFilteringTable->rules[i].flt_rule_hdl;
			}
			get_client_memptr(eth_client, clnt_indx)->ipv6_ul_rules_set = true;
		}
		else
		{
			IPACMERR("IP type is not expected.\n");
			goto fail;
		}
	}

fail:
	free(pFilteringTable);
	close(fd);
	return ret;
}

/* install UL filter rule from Q6 for all clients */
int IPACM_Lan::install_uplink_filter_rule
(
	ipacm_ext_prop* prop,
	ipa_ip_type iptype,
	uint8_t xlat_mux_id
)
{
	int ret = IPACM_SUCCESS, i=0;

	for (i = 0; i < num_eth_client; i++)
		{
			if (iptype == IPA_IP_v4)
			{
				if (get_client_memptr(eth_client, i)->ipv4_ul_rules_set == false)
				{
					ret = install_uplink_filter_rule_per_client(prop, iptype, xlat_mux_id, get_client_memptr(eth_client, i)->mac);
					IPACMDBG_H("IPA_IP_v4 xlat_mux_id: %d, modem_ul_v4_set %d\n", xlat_mux_id, get_client_memptr(eth_client, i)->ipv4_ul_rules_set);
				}
			}
			else if (iptype == IPA_IP_v6)
			{
				if (num_dft_rt_v6 ==1 && get_client_memptr(eth_client, i)->ipv6_ul_rules_set == false)
				{
					ret = install_uplink_filter_rule_per_client(prop, iptype, xlat_mux_id, get_client_memptr(eth_client, i)->mac);
					IPACMDBG_H("IPA_IP_v6 num_dft_rt_v6 %d xlat_mux_id: %d modem_ul_v6_set: %d\n", num_dft_rt_v6, xlat_mux_id, get_client_memptr(eth_client, i)->ipv6_ul_rules_set);
				}
			} else {
				IPACMDBG_H("ip-type: %d modem_ul_v4_set: %d, modem_ul_v6_set %d\n",
					iptype, get_client_memptr(eth_client, i)->ipv4_ul_rules_set, get_client_memptr(eth_client, i)->ipv6_ul_rules_set);

			}
		} /* end of for loop */

	return ret;
}

/* Delete UL filter rule from Q6 per client */
int IPACM_Lan::delete_uplink_filter_rule_per_client
(
	ipa_ip_type iptype,
	uint8_t *mac_addr
)
{
	int fd;
	int clnt_indx;

	fd = open(IPA_DEVICE_NAME, O_RDWR);
	if (0 == fd)
	{
		IPACMERR("Failed opening %s.\n", IPA_DEVICE_NAME);
		return IPACM_FAILURE;
	}

	clnt_indx = get_eth_client_index(mac_addr);

	if (clnt_indx == IPACM_INVALID_INDEX)
	{
		IPACMERR("eth client not found/attached \n");
		return IPACM_FAILURE;
	}

	if (get_client_memptr(eth_client, clnt_indx)->lan_stats_idx == -1)
	{
		IPACMERR("Invalid LAN Stats idx for ethernet client:%d \n", clnt_indx);
		return IPACM_FAILURE;
	}

	if (((iptype == IPA_IP_v4) && num_wan_ul_fl_rule_v4 > MAX_WAN_UL_FILTER_RULES) ||
		((iptype == IPA_IP_v6) && num_wan_ul_fl_rule_v6 > MAX_WAN_UL_FILTER_RULES))
	{
		IPACMERR("number of wan_ul_fl_rule_v4 (%d)/wan_ul_fl_rule_v6 (%d) > MAX_WAN_UL_FILTER_RULES (%d), aborting...\n",
			num_wan_ul_fl_rule_v4,
			num_wan_ul_fl_rule_v6,
			MAX_WAN_UL_FILTER_RULES);
		return IPACM_FAILURE;
	}

	if ((iptype == IPA_IP_v4) && get_client_memptr(eth_client, clnt_indx)->ipv4_ul_rules_set)
	{
		if (m_filtering.DeleteFilteringHdls(get_client_memptr(eth_client, clnt_indx)->wan_ul_fl_rule_hdl_v4,
				iptype, num_wan_ul_fl_rule_v4) == false)
		{
			IPACMERR("Error Deleting RuleTable(1) to Filtering, aborting...\n");
			close(fd);
			return IPACM_FAILURE;
		}
		memset(get_client_memptr(eth_client, clnt_indx)->wan_ul_fl_rule_hdl_v4, 0, MAX_WAN_UL_FILTER_RULES * sizeof(uint32_t));
		get_client_memptr(eth_client, clnt_indx)->ipv4_ul_rules_set = false;
	}

	if ((iptype == IPA_IP_v6) && get_client_memptr(eth_client, clnt_indx)->ipv6_ul_rules_set)
	{
		if (m_filtering.DeleteFilteringHdls(get_client_memptr(eth_client, clnt_indx)->wan_ul_fl_rule_hdl_v6,
				iptype, num_wan_ul_fl_rule_v6) == false)
		{
			IPACMERR("Error Deleting RuleTable(1) to Filtering, aborting...\n");
			close(fd);
			return IPACM_FAILURE;
		}
		memset(get_client_memptr(eth_client, clnt_indx)->wan_ul_fl_rule_hdl_v6, 0, MAX_WAN_UL_FILTER_RULES * sizeof(uint32_t));
		get_client_memptr(eth_client, clnt_indx)->ipv6_ul_rules_set = false;
	}
	return IPACM_SUCCESS;
}

/* Delete UL filter rule from Q6 for all clients */
int IPACM_Lan::delete_uplink_filter_rule
(
	ipa_ip_type iptype
)
{
	int ret = IPACM_SUCCESS, i=0;

	for (i = 0; i < num_eth_client; i++)
	{
		if (iptype == IPA_IP_v4)
		{
			if (get_client_memptr(eth_client, i)->ipv4_ul_rules_set == true)
			{
				IPACMDBG_H("IPA_IP_v4 Client id: %d, modem_ul_v4_set %d\n", i, get_client_memptr(eth_client, i)->ipv4_ul_rules_set);
				ret = delete_uplink_filter_rule_per_client(iptype, get_client_memptr(eth_client, i)->mac);
			}
		}
		else if (iptype == IPA_IP_v6)
		{
			if (get_client_memptr(eth_client, i)->ipv6_ul_rules_set == true)
			{
				IPACMDBG_H("IPA_IP_v6 Cliend id: %d modem_ul_v6_set: %d\n", i, get_client_memptr(eth_client, i)->ipv6_ul_rules_set);
				ret = delete_uplink_filter_rule_per_client(iptype, get_client_memptr(eth_client, i)->mac);
			}
		} else {
			ret = IPACM_FAILURE;
			IPACMDBG_H("ip-type: %d lan_stats_idx: %d modem_ul_v4_set: %d, modem_ul_v6_set %d\n",
				iptype, get_client_memptr(eth_client, i)->lan_stats_idx, get_client_memptr(eth_client, i)->ipv4_ul_rules_set, get_client_memptr(eth_client, i)->ipv6_ul_rules_set);
		}
	} /* end of for loop */

	return ret;
}

/* Set lan client info. */
int IPACM_Lan::set_lan_client_info(struct wan_ioctl_lan_client_info *client_info)
{
	int ret = IPACM_SUCCESS;
	int fd_wwan_ioctl;

	if (client_info == NULL)
	{
		IPACMERR("Client info NULL.\n");
		return IPACM_FAILURE;
	}

	IPACMDBG_H("Client MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					 client_info->mac[0], client_info->mac[1], client_info->mac[2],
					 client_info->mac[3], client_info->mac[4], client_info->mac[5]);

	fd_wwan_ioctl = open(WWAN_QMI_IOCTL_DEVICE_NAME, O_RDWR);

	if(fd_wwan_ioctl < 0)
	{
		IPACMERR("Failed to open %s.\n",WWAN_QMI_IOCTL_DEVICE_NAME);
		return IPACM_FAILURE;
	}

	ret = ioctl(fd_wwan_ioctl, WAN_IOC_SET_LAN_CLIENT_INFO, client_info);
	if (ret != 0)
	{
		IPACMERR("Failed to set client info %p\n ", client_info);
	}
	IPACMDBG("Set Client info: %p\n", client_info);
	close(fd_wwan_ioctl);
	return ret;
}

/* Clear lan client info. */
int IPACM_Lan::clear_lan_client_info(struct wan_ioctl_lan_client_info *client_info)
{
	int ret = IPACM_SUCCESS;
	int fd_wwan_ioctl;

	if (client_info == NULL)
	{
		IPACMERR("Client info NULL.\n");
		return IPACM_FAILURE;
	}

	IPACMDBG_H("Client MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					 client_info->mac[0], client_info->mac[1], client_info->mac[2],
					 client_info->mac[3], client_info->mac[4], client_info->mac[5]);

	fd_wwan_ioctl = open(WWAN_QMI_IOCTL_DEVICE_NAME, O_RDWR);

	if(fd_wwan_ioctl < 0)
	{
		IPACMERR("Failed to open %s.\n",WWAN_QMI_IOCTL_DEVICE_NAME);
		return IPACM_FAILURE;
	}

	ret = ioctl(fd_wwan_ioctl, WAN_IOC_CLEAR_LAN_CLIENT_INFO, client_info);
	if (ret != 0)
	{
		IPACMERR("Failed to set client info %p\n ", client_info);
	}
	IPACMDBG("Set Client info: %p\n", client_info);
	close(fd_wwan_ioctl);
	return ret;
}

/* Enable per client stats. */
int IPACM_Lan::enable_per_client_stats(bool *status)
{
	int ret = IPACM_SUCCESS;
	int fd_wwan_ioctl;

	if (status == NULL)
	{
		IPACMERR("Status is NULL.\n");
		return IPACM_FAILURE;
	}

	fd_wwan_ioctl = open(WWAN_QMI_IOCTL_DEVICE_NAME, O_RDWR);

	if(fd_wwan_ioctl < 0)
	{
		IPACMERR("Failed to open %s.\n",WWAN_QMI_IOCTL_DEVICE_NAME);
		return IPACM_FAILURE;
	}

	ret = ioctl(fd_wwan_ioctl, WAN_IOC_ENABLE_PER_CLIENT_STATS, status);
	if (ret != 0)
	{
		IPACMERR("Failed to enable per client stats %p\n ", status);
	}
	IPACMDBG("Enabled per client stats: %p\n", status);
	close(fd_wwan_ioctl);
	return ret;
}
#endif
int IPACM_Lan::handle_wan_down_v6(bool is_sta_mode)
{
	ipa_fltr_installed_notif_req_msg_v01 flt_index;
	int fd;

	fd = open(IPA_DEVICE_NAME, O_RDWR);
	if (0 == fd)
	{
		IPACMERR("Failed opening %s.\n", IPA_DEVICE_NAME);
		return IPACM_FAILURE;
	}

	delete_ipv6_prefix_flt_rule();

	memset(ipv6_prefix, 0, sizeof(ipv6_prefix));

	if(is_sta_mode == false)
	{
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
		if (IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable == false)
#endif
		{
			if (num_wan_ul_fl_rule_v6 > MAX_WAN_UL_FILTER_RULES)
			{
				IPACMERR(" the number of rules (%d) are bigger than array (%d), aborting...\n", num_wan_ul_fl_rule_v6, MAX_WAN_UL_FILTER_RULES);
				close(fd);
				return IPACM_FAILURE;
			}
			if (num_wan_ul_fl_rule_v6 == 0)
			{
				IPACMERR("No modem UL rules were installed, return...\n");
				close(fd);
				return IPACM_FAILURE;
			}

			if (m_filtering.DeleteFilteringHdls(wan_ul_fl_rule_hdl_v6,
				IPA_IP_v6, num_wan_ul_fl_rule_v6) == false)
			{
				IPACMERR("Error Deleting RuleTable(1) to Filtering, aborting...\n");
				close(fd);
				return IPACM_FAILURE;
			}
			IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, num_wan_ul_fl_rule_v6);
			memset(wan_ul_fl_rule_hdl_v6, 0, MAX_WAN_UL_FILTER_RULES * sizeof(uint32_t));
			num_wan_ul_fl_rule_v6 = 0;
		}
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
		else {
			if (delete_uplink_filter_rule(IPA_IP_v6) == IPACM_FAILURE)
			{
				IPACMERR("Error delete_uplink_filter_rule, aborting...\n");
				close(fd);
				return IPACM_FAILURE;
			}
			num_wan_ul_fl_rule_v6 = 0;
		}
#endif
		modem_ul_v6_set = false;

		memset(&flt_index, 0, sizeof(flt_index));
		flt_index.source_pipe_index = ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, rx_prop->rx[0].src_pipe);
		flt_index.install_status = IPA_QMI_RESULT_SUCCESS_V01;
#ifndef FEATURE_IPA_V3
		flt_index.filter_index_list_len = 0;
#else /* defined (FEATURE_IPA_V3) */
		flt_index.rule_id_valid = 1;
		flt_index.rule_id_len = 0;
#endif
		flt_index.embedded_pipe_index_valid = 1;
		flt_index.embedded_pipe_index = ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, IPA_CLIENT_APPS_LAN_WAN_PROD);
		flt_index.retain_header_valid = 1;
		flt_index.retain_header = 0;
		flt_index.embedded_call_mux_id_valid = 1;
		flt_index.embedded_call_mux_id = IPACM_Iface::ipacmcfg->GetQmapId();
		if(false == m_filtering.SendFilteringRuleIndex(&flt_index))
		{
			IPACMERR("Error sending filtering rule index, aborting...\n");
			close(fd);
			return IPACM_FAILURE;
		}
	}
	else
	{
		if (m_filtering.DeleteFilteringHdls(&dft_v6fl_rule_hdl[IPV6_DEFAULT_FILTERTING_RULES],
																				IPA_IP_v6, 1) == false)
		{
			IPACMERR("Error Adding RuleTable(1) to Filtering, aborting...\n");
			close(fd);
			return IPACM_FAILURE;
		}
		IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, 1);
	}
	close(fd);
	return IPACM_SUCCESS;
}

int IPACM_Lan::reset_to_dummy_flt_rule(ipa_ip_type iptype, uint32_t rule_hdl)
{
	int len, res = IPACM_SUCCESS;
	struct ipa_flt_rule_mdfy flt_rule;
	struct ipa_ioc_mdfy_flt_rule* pFilteringTable;

	IPACMDBG_H("Reset flt rule to dummy, IP type: %d, hdl: %d\n", iptype, rule_hdl);
	len = sizeof(struct ipa_ioc_mdfy_flt_rule) + sizeof(struct ipa_flt_rule_mdfy);
	pFilteringTable = (struct ipa_ioc_mdfy_flt_rule*)malloc(len);

	if (pFilteringTable == NULL)
	{
		IPACMERR("Error allocate flt rule memory...\n");
		return IPACM_FAILURE;
	}
	memset(pFilteringTable, 0, len);

	pFilteringTable->commit = 1;
	pFilteringTable->ip = iptype;
	pFilteringTable->num_rules = 1;

	memset(&flt_rule, 0, sizeof(struct ipa_flt_rule_mdfy));
	flt_rule.status = -1;
	flt_rule.rule_hdl = rule_hdl;

	flt_rule.rule.retain_hdr = 0;
	flt_rule.rule.action = IPA_PASS_TO_EXCEPTION;

	if(iptype == IPA_IP_v4)
	{
		IPACMDBG_H("Reset IPv4 flt rule to dummy\n");

		flt_rule.rule.attrib.attrib_mask = IPA_FLT_SRC_ADDR | IPA_FLT_DST_ADDR;
		flt_rule.rule.attrib.u.v4.dst_addr = ~0;
		flt_rule.rule.attrib.u.v4.dst_addr_mask = ~0;
		flt_rule.rule.attrib.u.v4.src_addr = ~0;
		flt_rule.rule.attrib.u.v4.src_addr_mask = ~0;

		memcpy(&(pFilteringTable->rules[0]), &flt_rule, sizeof(struct ipa_flt_rule_mdfy));
		if (false == m_filtering.ModifyFilteringRule(pFilteringTable))
		{
			IPACMERR("Error modifying filtering rule.\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		else
		{
			IPACMDBG_H("Flt rule reset to dummy, hdl: 0x%x, status: %d\n", pFilteringTable->rules[0].rule_hdl,
						pFilteringTable->rules[0].status);
		}
	}
	else if(iptype == IPA_IP_v6)
	{
		IPACMDBG_H("Reset IPv6 flt rule to dummy\n");

		flt_rule.rule.attrib.attrib_mask = IPA_FLT_SRC_ADDR | IPA_FLT_DST_ADDR;
		flt_rule.rule.attrib.u.v6.src_addr[0] = ~0;
		flt_rule.rule.attrib.u.v6.src_addr[1] = ~0;
		flt_rule.rule.attrib.u.v6.src_addr[2] = ~0;
		flt_rule.rule.attrib.u.v6.src_addr[3] = ~0;
		flt_rule.rule.attrib.u.v6.src_addr_mask[0] = ~0;
		flt_rule.rule.attrib.u.v6.src_addr_mask[1] = ~0;
		flt_rule.rule.attrib.u.v6.src_addr_mask[2] = ~0;
		flt_rule.rule.attrib.u.v6.src_addr_mask[3] = ~0;
		flt_rule.rule.attrib.u.v6.dst_addr[0] = ~0;
		flt_rule.rule.attrib.u.v6.dst_addr[1] = ~0;
		flt_rule.rule.attrib.u.v6.dst_addr[2] = ~0;
		flt_rule.rule.attrib.u.v6.dst_addr[3] = ~0;
		flt_rule.rule.attrib.u.v6.dst_addr_mask[0] = ~0;
		flt_rule.rule.attrib.u.v6.dst_addr_mask[1] = ~0;
		flt_rule.rule.attrib.u.v6.dst_addr_mask[2] = ~0;
		flt_rule.rule.attrib.u.v6.dst_addr_mask[3] = ~0;


		memcpy(&(pFilteringTable->rules[0]), &flt_rule, sizeof(struct ipa_flt_rule_mdfy));
		if (false == m_filtering.ModifyFilteringRule(pFilteringTable))
		{
			IPACMERR("Error modifying filtering rule.\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		else
		{
			IPACMDBG_H("Flt rule reset to dummy, hdl: 0x%x, status: %d\n", pFilteringTable->rules[0].rule_hdl,
						pFilteringTable->rules[0].status);
		}
	}
	else
	{
		IPACMERR("IP type is not expected.\n");
		res = IPACM_FAILURE;
		goto fail;
	}

fail:
	free(pFilteringTable);
	return res;
}

void IPACM_Lan::post_del_self_evt()
{
	ipacm_cmd_q_data evt;
	ipacm_event_data_fid* fid;
	fid = (ipacm_event_data_fid*)malloc(sizeof(ipacm_event_data_fid));
	if(fid == NULL)
	{
		IPACMERR("Failed to allocate fid memory.\n");
		return;
	}
	memset(fid, 0, sizeof(ipacm_event_data_fid));
	memset(&evt, 0, sizeof(ipacm_cmd_q_data));

	fid->if_index = ipa_if_num;

	evt.evt_data = (void*)fid;
	evt.event = IPA_LAN_DELETE_SELF;

	IPACMDBG_H("Posting event IPA_LAN_DELETE_SELF\n");
	IPACM_EvtDispatcher::PostEvt(&evt);
}

/*handle reset usb-client rt-rules */
int IPACM_Lan::handle_lan_client_reset_rt(ipa_ip_type iptype)
{
	int i, res = IPACM_SUCCESS;

	/* clean eth-client routing rules */
	IPACMDBG_H("left %d eth clients need to be deleted \n ", num_eth_client);
	for (i = 0; i < num_eth_client; i++)
	{
		res = delete_eth_rtrules(i, iptype);
		if (res != IPACM_SUCCESS)
		{
			IPACMERR("Failed to delete old iptype(%d) rules.\n", iptype);
			return res;
		}
	} /* end of for loop */

	/* Reset ip-address */
	for (i = 0; i < num_eth_client; i++)
	{
		if(iptype == IPA_IP_v4)
		{
			get_client_memptr(eth_client, i)->ipv4_set = false;
		}
		else
		{
			get_client_memptr(eth_client, i)->ipv6_set = 0;
		}
	} /* end of for loop */
	return res;
}

int IPACM_Lan::install_ipv4_icmp_flt_rule()
{
	int len;
	struct ipa_ioc_add_flt_rule* flt_rule;
	struct ipa_flt_rule_add flt_rule_entry;

	if(rx_prop != NULL)
	{
		len = sizeof(struct ipa_ioc_add_flt_rule) + sizeof(struct ipa_flt_rule_add);

		flt_rule = (struct ipa_ioc_add_flt_rule *)calloc(1, len);
		if (!flt_rule)
		{
			IPACMERR("Error Locate ipa_flt_rule_add memory...\n");
			return IPACM_FAILURE;
		}

		flt_rule->commit = 1;
		flt_rule->ep = rx_prop->rx[0].src_pipe;
		flt_rule->global = false;
		flt_rule->ip = IPA_IP_v4;
		flt_rule->num_rules = 1;

		memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));

		flt_rule_entry.rule.retain_hdr = 1;
		flt_rule_entry.rule.to_uc = 0;
		flt_rule_entry.rule.eq_attrib_type = 0;
		flt_rule_entry.at_rear = true;
		flt_rule_entry.flt_rule_hdl = -1;
		flt_rule_entry.status = -1;
		flt_rule_entry.rule.action = IPA_PASS_TO_EXCEPTION;
#ifdef FEATURE_IPA_V3
		flt_rule_entry.rule.hashable = true;
#endif
		memcpy(&flt_rule_entry.rule.attrib, &rx_prop->rx[0].attrib, sizeof(flt_rule_entry.rule.attrib));

		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_PROTOCOL;
		flt_rule_entry.rule.attrib.u.v4.protocol = (uint8_t)IPACM_FIREWALL_IPPROTO_ICMP;
		memcpy(&(flt_rule->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));

		if (m_filtering.AddFilteringRule(flt_rule) == false)
		{
			IPACMERR("Error Adding Filtering rule, aborting...\n");
			free(flt_rule);
			return IPACM_FAILURE;
		}
		else
		{
			IPACM_Iface::ipacmcfg->increaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4, 1);
			ipv4_icmp_flt_rule_hdl[0] = flt_rule->rules[0].flt_rule_hdl;
			IPACMDBG_H("IPv4 icmp filter rule HDL:0x%x\n", ipv4_icmp_flt_rule_hdl[0]);
                        free(flt_rule);
		}
	}
	return IPACM_SUCCESS;
}

int IPACM_Lan::install_ipv6_icmp_flt_rule()
{

	int len;
	struct ipa_ioc_add_flt_rule* flt_rule;
	struct ipa_flt_rule_add flt_rule_entry;

	if(rx_prop != NULL)
	{
		len = sizeof(struct ipa_ioc_add_flt_rule) + sizeof(struct ipa_flt_rule_add);

		flt_rule = (struct ipa_ioc_add_flt_rule *)calloc(1, len);
		if (!flt_rule)
		{
			IPACMERR("Error Locate ipa_flt_rule_add memory...\n");
			return IPACM_FAILURE;
		}

		flt_rule->commit = 1;
		flt_rule->ep = rx_prop->rx[0].src_pipe;
		flt_rule->global = false;
		flt_rule->ip = IPA_IP_v6;
		flt_rule->num_rules = 1;

		memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));

		flt_rule_entry.rule.retain_hdr = 1;
		flt_rule_entry.rule.to_uc = 0;
		flt_rule_entry.rule.eq_attrib_type = 0;
		flt_rule_entry.at_rear = true;
		flt_rule_entry.flt_rule_hdl = -1;
		flt_rule_entry.status = -1;
		flt_rule_entry.rule.action = IPA_PASS_TO_EXCEPTION;
#ifdef FEATURE_IPA_V3
		flt_rule_entry.rule.hashable = false;
#endif
		memcpy(&flt_rule_entry.rule.attrib, &rx_prop->rx[0].attrib, sizeof(flt_rule_entry.rule.attrib));
		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_NEXT_HDR;
		flt_rule_entry.rule.attrib.u.v6.next_hdr = (uint8_t)IPACM_FIREWALL_IPPROTO_ICMP6;
		memcpy(&(flt_rule->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));

		if (m_filtering.AddFilteringRule(flt_rule) == false)
		{
			IPACMERR("Error Adding Filtering rule, aborting...\n");
			free(flt_rule);
			return IPACM_FAILURE;
		}
		else
		{
			IPACM_Iface::ipacmcfg->increaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, 1);
			ipv6_icmp_flt_rule_hdl[0] = flt_rule->rules[0].flt_rule_hdl;
			IPACMDBG_H("IPv6 icmp filter rule HDL:0x%x\n", ipv6_icmp_flt_rule_hdl[0]);
			free(flt_rule);
		}
	}
	return IPACM_SUCCESS;
}

int IPACM_Lan::add_dummy_private_subnet_flt_rule(ipa_ip_type iptype)
{
	if(rx_prop == NULL)
	{
		IPACMDBG_H("There is no rx_prop for iface %s, not able to add dummy private subnet filtering rule.\n", dev_name);
		return 0;
	}

	if(iptype == IPA_IP_v6)
	{
		IPACMDBG_H("There is no ipv6 dummy filter rules needed for iface %s\n", dev_name);
		return 0;
	}
	int i, len, res = IPACM_SUCCESS;
	struct ipa_flt_rule_add flt_rule;
	ipa_ioc_add_flt_rule* pFilteringTable;

	len = sizeof(struct ipa_ioc_add_flt_rule) +	IPA_MAX_PRIVATE_SUBNET_ENTRIES * sizeof(struct ipa_flt_rule_add);

	pFilteringTable = (struct ipa_ioc_add_flt_rule *)malloc(len);
	if (pFilteringTable == NULL)
	{
		IPACMERR("Error allocate flt table memory...\n");
		return IPACM_FAILURE;
	}
	memset(pFilteringTable, 0, len);

	pFilteringTable->commit = 1;
	pFilteringTable->ep = rx_prop->rx[0].src_pipe;
	pFilteringTable->global = false;
	pFilteringTable->ip = iptype;
	pFilteringTable->num_rules = IPA_MAX_PRIVATE_SUBNET_ENTRIES;

	memset(&flt_rule, 0, sizeof(struct ipa_flt_rule_add));

	flt_rule.rule.retain_hdr = 0;
	flt_rule.at_rear = true;
	flt_rule.flt_rule_hdl = -1;
	flt_rule.status = -1;
	flt_rule.rule.action = IPA_PASS_TO_EXCEPTION;
#ifdef FEATURE_IPA_V3
	flt_rule.rule.hashable = true;
#endif
	memcpy(&flt_rule.rule.attrib, &rx_prop->rx[0].attrib,
			sizeof(flt_rule.rule.attrib));

	if(iptype == IPA_IP_v4)
	{
		flt_rule.rule.attrib.attrib_mask = IPA_FLT_SRC_ADDR | IPA_FLT_DST_ADDR;
		flt_rule.rule.attrib.u.v4.src_addr_mask = ~0;
		flt_rule.rule.attrib.u.v4.src_addr = ~0;
		flt_rule.rule.attrib.u.v4.dst_addr_mask = ~0;
		flt_rule.rule.attrib.u.v4.dst_addr = ~0;

		for(i=0; i<IPA_MAX_PRIVATE_SUBNET_ENTRIES; i++)
		{
			memcpy(&(pFilteringTable->rules[i]), &flt_rule, sizeof(struct ipa_flt_rule_add));
		}

		if (false == m_filtering.AddFilteringRule(pFilteringTable))
		{
			IPACMERR("Error adding dummy private subnet v4 flt rule\n");
			res = IPACM_FAILURE;
			goto fail;
		}
		else
		{
			IPACM_Iface::ipacmcfg->increaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v4, IPA_MAX_PRIVATE_SUBNET_ENTRIES);
			/* copy filter rule hdls */
			for (int i = 0; i < IPA_MAX_PRIVATE_SUBNET_ENTRIES; i++)
			{
				if (pFilteringTable->rules[i].status == 0)
				{
					private_fl_rule_hdl[i] = pFilteringTable->rules[i].flt_rule_hdl;
					IPACMDBG_H("Private subnet v4 flt rule %d hdl:0x%x\n", i, private_fl_rule_hdl[i]);
				}
				else
				{
					IPACMERR("Failed adding lan2lan v4 flt rule %d\n", i);
					res = IPACM_FAILURE;
					goto fail;
				}
			}
		}
	}
fail:
	free(pFilteringTable);
	return res;
}

int IPACM_Lan::handle_private_subnet_android(ipa_ip_type iptype)
{
	int i, len, res = IPACM_SUCCESS;
	struct ipa_flt_rule_mdfy flt_rule;
	struct ipa_ioc_mdfy_flt_rule* pFilteringTable;

	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	if(iptype == IPA_IP_v6)
	{
		IPACMDBG_H("There is no ipv6 dummy filter rules needed for iface %s\n", dev_name);
		return 0;
	}
	else
	{
		for(i=0; i<IPA_MAX_PRIVATE_SUBNET_ENTRIES; i++)
		{
			reset_to_dummy_flt_rule(IPA_IP_v4, private_fl_rule_hdl[i]);
		}

		len = sizeof(struct ipa_ioc_mdfy_flt_rule) + (IPACM_Iface::ipacmcfg->ipa_num_private_subnet) * sizeof(struct ipa_flt_rule_mdfy);
		pFilteringTable = (struct ipa_ioc_mdfy_flt_rule*)malloc(len);
		if (!pFilteringTable)
		{
			IPACMERR("Failed to allocate ipa_ioc_mdfy_flt_rule memory...\n");
			return IPACM_FAILURE;
		}
		memset(pFilteringTable, 0, len);

		pFilteringTable->commit = 1;
		pFilteringTable->ip = iptype;
		pFilteringTable->num_rules = (uint8_t)IPACM_Iface::ipacmcfg->ipa_num_private_subnet;

		/* Make LAN-traffic always go A5, use default IPA-RT table */
		if (false == m_routing.GetRoutingTable(&IPACM_Iface::ipacmcfg->rt_tbl_default_v4))
		{
			IPACMERR("Failed to get routing table handle.\n");
			res = IPACM_FAILURE;
			goto fail;
		}

		memset(&flt_rule, 0, sizeof(struct ipa_flt_rule_mdfy));
		flt_rule.status = -1;

		flt_rule.rule.retain_hdr = 1;
		flt_rule.rule.to_uc = 0;
		flt_rule.rule.action = IPA_PASS_TO_ROUTING;
		flt_rule.rule.eq_attrib_type = 0;
		flt_rule.rule.rt_tbl_hdl = IPACM_Iface::ipacmcfg->rt_tbl_default_v4.hdl;
		IPACMDBG_H("Private filter rule use table: %s\n",IPACM_Iface::ipacmcfg->rt_tbl_default_v4.name);

		memcpy(&flt_rule.rule.attrib, &rx_prop->rx[0].attrib, sizeof(flt_rule.rule.attrib));
		flt_rule.rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;

		for (i = 0; i < (IPACM_Iface::ipacmcfg->ipa_num_private_subnet); i++)
		{
			flt_rule.rule_hdl = private_fl_rule_hdl[i];
			flt_rule.rule.attrib.u.v4.dst_addr_mask = IPACM_Iface::ipacmcfg->private_subnet_table[i].subnet_mask;
			flt_rule.rule.attrib.u.v4.dst_addr = IPACM_Iface::ipacmcfg->private_subnet_table[i].subnet_addr;
			memcpy(&(pFilteringTable->rules[i]), &flt_rule, sizeof(struct ipa_flt_rule_mdfy));
			IPACMDBG_H(" IPACM private subnet_addr as: 0x%x entry(%d)\n", flt_rule.rule.attrib.u.v4.dst_addr, i);
		}

		if (false == m_filtering.ModifyFilteringRule(pFilteringTable))
		{
			IPACMERR("Failed to modify private subnet filtering rules.\n");
			res = IPACM_FAILURE;
			goto fail;
		}
	}
fail:
	if(pFilteringTable != NULL)
	{
		free(pFilteringTable);
	}
	return res;
}

int IPACM_Lan::install_ipv6_prefix_flt_rule(uint32_t* prefix)
{
	if(prefix == NULL)
	{
		IPACMERR("IPv6 prefix is empty.\n");
		return IPACM_FAILURE;
	}
	IPACMDBG_H("Receive IPv6 prefix: 0x%08x%08x.\n", prefix[0], prefix[1]);

	int len;
	struct ipa_ioc_add_flt_rule* flt_rule;
	struct ipa_flt_rule_add flt_rule_entry;

	if(rx_prop != NULL)
	{
		len = sizeof(struct ipa_ioc_add_flt_rule) + sizeof(struct ipa_flt_rule_add);

		flt_rule = (struct ipa_ioc_add_flt_rule *)calloc(1, len);
		if (!flt_rule)
		{
			IPACMERR("Error Locate ipa_flt_rule_add memory...\n");
			return IPACM_FAILURE;
		}

		flt_rule->commit = 1;
		flt_rule->ep = rx_prop->rx[0].src_pipe;
		flt_rule->global = false;
		flt_rule->ip = IPA_IP_v6;
		flt_rule->num_rules = 1;

		memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));

		flt_rule_entry.rule.retain_hdr = 1;
		flt_rule_entry.rule.to_uc = 0;
		flt_rule_entry.rule.eq_attrib_type = 0;
		flt_rule_entry.at_rear = true;
		flt_rule_entry.flt_rule_hdl = -1;
		flt_rule_entry.status = -1;
		flt_rule_entry.rule.action = IPA_PASS_TO_EXCEPTION;
#ifdef FEATURE_IPA_V3
		flt_rule_entry.rule.hashable = true;
#endif
		memcpy(&flt_rule_entry.rule.attrib, &rx_prop->rx[0].attrib, sizeof(flt_rule_entry.rule.attrib));
		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
		flt_rule_entry.rule.attrib.u.v6.dst_addr[0] = prefix[0];
		flt_rule_entry.rule.attrib.u.v6.dst_addr[1] = prefix[1];
		flt_rule_entry.rule.attrib.u.v6.dst_addr[2] = 0x0;
		flt_rule_entry.rule.attrib.u.v6.dst_addr[3] = 0x0;
		flt_rule_entry.rule.attrib.u.v6.dst_addr_mask[0] = 0xFFFFFFFF;
		flt_rule_entry.rule.attrib.u.v6.dst_addr_mask[1] = 0xFFFFFFFF;
		flt_rule_entry.rule.attrib.u.v6.dst_addr_mask[2] = 0x0;
		flt_rule_entry.rule.attrib.u.v6.dst_addr_mask[3] = 0x0;
		memcpy(&(flt_rule->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));

		if (m_filtering.AddFilteringRule(flt_rule) == false)
		{
			IPACMERR("Error Adding Filtering rule, aborting...\n");
			free(flt_rule);
			return IPACM_FAILURE;
		}
		else
		{
			IPACM_Iface::ipacmcfg->increaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, 1);
			ipv6_prefix_flt_rule_hdl[0] = flt_rule->rules[0].flt_rule_hdl;
			IPACMDBG_H("IPv6 prefix filter rule HDL:0x%x\n", ipv6_prefix_flt_rule_hdl[0]);
			free(flt_rule);
		}
	}
	return IPACM_SUCCESS;
}

void IPACM_Lan::delete_ipv6_prefix_flt_rule()
{
	if(m_filtering.DeleteFilteringHdls(ipv6_prefix_flt_rule_hdl, IPA_IP_v6, NUM_IPV6_PREFIX_FLT_RULE) == false)
	{
		IPACMERR("Failed to delete ipv6 prefix flt rule.\n");
		return;
	}
	IPACM_Iface::ipacmcfg->decreaseFltRuleCount(rx_prop->rx[0].src_pipe, IPA_IP_v6, NUM_IPV6_PREFIX_FLT_RULE);
	return;
}

int IPACM_Lan::handle_addr_evt_odu_bridge(ipacm_event_data_addr* data)
{
	int fd, res = IPACM_SUCCESS;
	struct in6_addr ipv6_addr;
	if(data == NULL)
	{
		IPACMERR("Failed to get interface IP address.\n");
		return IPACM_FAILURE;
	}

	if(data->iptype == IPA_IP_v6)
	{
		fd = open(IPACM_Iface::ipacmcfg->DEVICE_NAME_ODU, O_RDWR);
		if(fd == 0)
		{
			IPACMERR("Failed to open %s.\n", IPACM_Iface::ipacmcfg->DEVICE_NAME_ODU);
			return IPACM_FAILURE;
		}

		memcpy(&ipv6_addr, data->ipv6_addr, sizeof(struct in6_addr));

		if( ioctl(fd, ODU_BRIDGE_IOC_SET_LLV6_ADDR, &ipv6_addr) )
		{
			IPACMERR("Failed to write IPv6 address to odu driver.\n");
			res = IPACM_FAILURE;
		}
		num_dft_rt_v6++;
		close(fd);
	}

	return res;
}

ipa_hdr_proc_type IPACM_Lan::eth_bridge_get_hdr_proc_type(ipa_hdr_l2_type t1, ipa_hdr_l2_type t2)
{
	if(t1 == IPA_HDR_L2_ETHERNET_II)
	{
		if(t2 == IPA_HDR_L2_ETHERNET_II)
		{
			return IPA_HDR_PROC_ETHII_TO_ETHII;
		}
		if(t2 == IPA_HDR_L2_802_3)
		{
			return IPA_HDR_PROC_ETHII_TO_802_3;
		}
	}

	if(t1 == IPA_HDR_L2_802_3)
	{
		if(t2 == IPA_HDR_L2_ETHERNET_II)
		{
			return IPA_HDR_PROC_802_3_TO_ETHII;
		}
		if(t2 == IPA_HDR_L2_802_3)
		{
			return IPA_HDR_PROC_802_3_TO_802_3;
		}
	}

	return IPA_HDR_PROC_NONE;
}

int IPACM_Lan::eth_bridge_get_hdr_template_hdl(uint32_t* hdr_hdl)
{
	if(hdr_hdl == NULL)
	{
		IPACMDBG_H("Hdr handle pointer is empty.\n");
		return IPACM_FAILURE;
	}

	struct ipa_ioc_get_hdr hdr;
	memset(&hdr, 0, sizeof(hdr));

	memcpy(hdr.name, tx_prop->tx[0].hdr_name, sizeof(hdr.name));
	if(m_header.GetHeaderHandle(&hdr) == false)
	{
		IPACMERR("Failed to get template hdr hdl.\n");
		return IPACM_FAILURE;
	}

	*hdr_hdl = hdr.hdl;
	return IPACM_SUCCESS;
}

int IPACM_Lan::handle_cradle_wan_mode_switch(bool is_wan_bridge_mode)
{
	struct ipa_flt_rule_mdfy flt_rule_entry;
	int len = 0;
	ipa_ioc_mdfy_flt_rule *m_pFilteringTable;

	IPACMDBG_H("Handle wan mode swtich: is wan bridge mode?%d\n", is_wan_bridge_mode);

	if (rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	len = sizeof(struct ipa_ioc_mdfy_flt_rule) + (1 * sizeof(struct ipa_flt_rule_mdfy));
	m_pFilteringTable = (struct ipa_ioc_mdfy_flt_rule *)calloc(1, len);
	if (m_pFilteringTable == NULL)
	{
		PERROR("Error Locate ipa_ioc_mdfy_flt_rule memory...\n");
		return IPACM_FAILURE;
	}

	m_pFilteringTable->commit = 1;
	m_pFilteringTable->ip = IPA_IP_v4;
	m_pFilteringTable->num_rules = (uint8_t)1;

	IPACMDBG_H("Retrieving routing hanle for table: %s\n",
					 IPACM_Iface::ipacmcfg->rt_tbl_wan_v4.name);
	if (false == m_routing.GetRoutingTable(&IPACM_Iface::ipacmcfg->rt_tbl_wan_v4))
	{
		IPACMERR("m_routing.GetRoutingTable(&IPACM_Iface::ipacmcfg->rt_tbl_wan_v4=0x%p) Failed.\n",
						 &IPACM_Iface::ipacmcfg->rt_tbl_wan_v4);
		free(m_pFilteringTable);
		return IPACM_FAILURE;
	}
	IPACMDBG_H("Routing handle for table: %d\n", IPACM_Iface::ipacmcfg->rt_tbl_wan_v4.hdl);


	memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_mdfy)); // Zero All Fields
	flt_rule_entry.status = -1;
	flt_rule_entry.rule_hdl = lan_wan_fl_rule_hdl[0];

	flt_rule_entry.rule.retain_hdr = 0;
	flt_rule_entry.rule.to_uc = 0;
	flt_rule_entry.rule.eq_attrib_type = 0;
	if(is_wan_bridge_mode)
	{
		flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
	}
	else
	{
		flt_rule_entry.rule.action = IPA_PASS_TO_SRC_NAT;
	}
	flt_rule_entry.rule.rt_tbl_hdl = IPACM_Iface::ipacmcfg->rt_tbl_wan_v4.hdl;

	memcpy(&flt_rule_entry.rule.attrib,
				 &rx_prop->rx[0].attrib,
				 sizeof(flt_rule_entry.rule.attrib));

	flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
	flt_rule_entry.rule.attrib.u.v4.dst_addr_mask = 0x0;
	flt_rule_entry.rule.attrib.u.v4.dst_addr = 0x0;

	memcpy(&m_pFilteringTable->rules[0], &flt_rule_entry, sizeof(flt_rule_entry));
	if (false == m_filtering.ModifyFilteringRule(m_pFilteringTable))
	{
		IPACMERR("Error Modifying RuleTable(0) to Filtering, aborting...\n");
		free(m_pFilteringTable);
		return IPACM_FAILURE;
	}
	else
	{
		IPACMDBG_H("flt rule hdl = %d, status = %d\n",
						 m_pFilteringTable->rules[0].rule_hdl,
						 m_pFilteringTable->rules[0].status);
	}
	free(m_pFilteringTable);
	return IPACM_SUCCESS;
}

/*handle reset usb-client rt-rules */
int IPACM_Lan::handle_tethering_stats_event(ipa_get_data_stats_resp_msg_v01 *data)
{
	int cnt, pipe_len, fd;
	uint64_t num_ul_packets, num_ul_bytes;
	uint64_t num_dl_packets, num_dl_bytes;
	bool ul_pipe_found, dl_pipe_found;
	FILE *fp = NULL;

	fd = open(IPA_DEVICE_NAME, O_RDWR);
	if (fd < 0)
	{
		IPACMERR("Failed opening %s.\n", IPA_DEVICE_NAME);
		return IPACM_FAILURE;
	}


	ul_pipe_found = false;
	dl_pipe_found = false;
	num_ul_packets = 0;
	num_dl_packets = 0;
	num_ul_bytes = 0;
	num_dl_bytes = 0;

	if (data->dl_dst_pipe_stats_list_valid)
	{
		if(tx_prop != NULL)
		{
			for (pipe_len = 0; pipe_len < data->dl_dst_pipe_stats_list_len; pipe_len++)
			{
				IPACMDBG_H("Check entry(%d) dl_dst_pipe(%d)\n", pipe_len, data->dl_dst_pipe_stats_list[pipe_len].pipe_index);
				for (cnt=0; cnt<tx_prop->num_tx_props; cnt++)
				{
					IPACMDBG_H("Check Tx_prop_entry(%d) pipe(%d)\n", cnt, ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, tx_prop->tx[cnt].dst_pipe));
					if(ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, tx_prop->tx[cnt].dst_pipe) == data->dl_dst_pipe_stats_list[pipe_len].pipe_index)
					{
						/* update the DL stats */
						dl_pipe_found = true;
						num_dl_packets += data->dl_dst_pipe_stats_list[pipe_len].num_ipv4_packets;
						num_dl_packets += data->dl_dst_pipe_stats_list[pipe_len].num_ipv6_packets;
						num_dl_bytes += data->dl_dst_pipe_stats_list[pipe_len].num_ipv4_bytes;
						num_dl_bytes += data->dl_dst_pipe_stats_list[pipe_len].num_ipv6_bytes;
						IPACMDBG_H("Got matched dst-pipe (%d) from %d tx props\n", data->dl_dst_pipe_stats_list[pipe_len].pipe_index, cnt);
						IPACMDBG_H("DL_packets:(%lu) DL_bytes:(%lu) \n", num_dl_packets, num_dl_bytes);
						break;
					}
				}
			}
		}
	}

	if (data->ul_src_pipe_stats_list_valid)
	{
		if(rx_prop != NULL)
		{
			for (pipe_len = 0; pipe_len < data->ul_src_pipe_stats_list_len; pipe_len++)
			{
				IPACMDBG_H("Check entry(%d) dl_dst_pipe(%d)\n", pipe_len, data->ul_src_pipe_stats_list[pipe_len].pipe_index);
				for (cnt=0; cnt < rx_prop->num_rx_props; cnt++)
				{
					IPACMDBG_H("Check Rx_prop_entry(%d) pipe(%d)\n", cnt, ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, rx_prop->rx[cnt].src_pipe));
					if(ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, rx_prop->rx[cnt].src_pipe) == data->ul_src_pipe_stats_list[pipe_len].pipe_index)
					{
						/* update the UL stats */
						ul_pipe_found = true;
						num_ul_packets += data->ul_src_pipe_stats_list[pipe_len].num_ipv4_packets;
						num_ul_packets += data->ul_src_pipe_stats_list[pipe_len].num_ipv6_packets;
						num_ul_bytes += data->ul_src_pipe_stats_list[pipe_len].num_ipv4_bytes;
						num_ul_bytes += data->ul_src_pipe_stats_list[pipe_len].num_ipv6_bytes;
						IPACMDBG_H("Got matched dst-pipe (%d) from %d tx props\n", data->ul_src_pipe_stats_list[pipe_len].pipe_index, cnt);
						IPACMDBG_H("UL_packets:(%lu) UL_bytes:(%lu) \n", num_ul_packets, num_ul_bytes);
						break;
					}
				}
			}
		}
	}
	close(fd);

	if (ul_pipe_found || dl_pipe_found)
	{
		IPACMDBG_H("Update IPA_TETHERING_STATS_UPDATE_EVENT, TX(P%lu/B%lu) RX(P%lu/B%lu) DEV(%s) to LTE(%s) \n",
					num_ul_packets,
						num_ul_bytes,
							num_dl_packets,
								num_dl_bytes,
									dev_name,
										IPACM_Wan::wan_up_dev_name);
		fp = fopen(IPA_PIPE_STATS_FILE_NAME, "w");
		if ( fp == NULL )
		{
			IPACMERR("Failed to write pipe stats to %s, error is %d - %s\n",
					IPA_PIPE_STATS_FILE_NAME, errno, strerror(errno));
			return IPACM_FAILURE;
		}

		fprintf(fp, PIPE_STATS,
				dev_name,
					IPACM_Wan::wan_up_dev_name,
						num_ul_bytes,
						num_ul_packets,
							    num_dl_bytes,
							num_dl_packets);
		fclose(fp);
	}
	return IPACM_SUCCESS;
}

/*handle tether client */
int IPACM_Lan::handle_tethering_client(bool reset, ipacm_client_enum ipa_client)
{
	int cnt, fd, ret = IPACM_SUCCESS;
	int fd_wwan_ioctl = open(WWAN_QMI_IOCTL_DEVICE_NAME, O_RDWR);
	wan_ioctl_set_tether_client_pipe tether_client;

	if(fd_wwan_ioctl < 0)
	{
		IPACMERR("Failed to open %s.\n",WWAN_QMI_IOCTL_DEVICE_NAME);
		return IPACM_FAILURE;
	}

	fd = open(IPA_DEVICE_NAME, O_RDWR);
	if (fd < 0)
	{
		IPACMERR("Failed opening %s.\n", IPA_DEVICE_NAME);
		close(fd_wwan_ioctl);
		return IPACM_FAILURE;
	}

	memset(&tether_client, 0, sizeof(tether_client));
	tether_client.reset_client = reset;
	tether_client.ipa_client = ipa_client;

	if(tx_prop != NULL)
	{
		tether_client.dl_dst_pipe_len = tx_prop->num_tx_props;
		for (cnt = 0; cnt < tx_prop->num_tx_props; cnt++)
		{
			IPACMDBG_H("Tx(%d), dst_pipe: %d, ipa_pipe: %d\n",
					cnt, tx_prop->tx[cnt].dst_pipe,
						ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, tx_prop->tx[cnt].dst_pipe));
			tether_client.dl_dst_pipe_list[cnt] = ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, tx_prop->tx[cnt].dst_pipe);
		}
	}

	if(rx_prop != NULL)
	{
		tether_client.ul_src_pipe_len = rx_prop->num_rx_props;
		for (cnt = 0; cnt < rx_prop->num_rx_props; cnt++)
		{
			IPACMDBG_H("Rx(%d), src_pipe: %d, ipa_pipe: %d\n",
					cnt, rx_prop->rx[cnt].src_pipe,
						ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, rx_prop->rx[cnt].src_pipe));
			tether_client.ul_src_pipe_list[cnt] = ioctl(fd, IPA_IOC_QUERY_EP_MAPPING, rx_prop->rx[cnt].src_pipe);
		}
	}

	ret = ioctl(fd_wwan_ioctl, WAN_IOC_SET_TETHER_CLIENT_PIPE, &tether_client);
	if (ret != 0)
	{
		IPACMERR("Failed set tether-client-pipe %p with ret %d\n ", &tether_client, ret);
	}
	IPACMDBG("Set tether-client-pipe %p\n", &tether_client);
	close(fd);
	close(fd_wwan_ioctl);
	return ret;
}

/* mac address has to be provided for client related events */
void IPACM_Lan::eth_bridge_post_event(ipa_cm_event_id evt, ipa_ip_type iptype, uint8_t *mac, uint32_t *ipv6_addr, char *iface_name)
{
	ipacm_cmd_q_data eth_bridge_evt;
	ipacm_event_eth_bridge *evt_data_eth_bridge;
	ipacm_event_data_all *evt_data_all;

	memset(&eth_bridge_evt, 0, sizeof(ipacm_cmd_q_data));
	eth_bridge_evt.event = evt;

	if(evt == IPA_HANDLE_VLAN_CLIENT_INFO || evt == IPA_HANDLE_VLAN_IFACE_INFO)
	{
		evt_data_all = (ipacm_event_data_all*)malloc(sizeof(*evt_data_all));
		if(evt_data_all == NULL)
		{
			IPACMERR("Failed to allocate memory.\n");
			return;
		}
		memset(evt_data_all, 0, sizeof(*evt_data_all));

		if(ipv6_addr)
		{
			IPACMDBG_H("IPv6 addr: %08x:%08x:%08x:%08x \n", ipv6_addr[0],
				ipv6_addr[1], ipv6_addr[2], ipv6_addr[3]);
			memcpy(evt_data_all->ipv6_addr, ipv6_addr, sizeof(evt_data_all->ipv6_addr));
		}
		if(mac)
		{
			IPACMDBG_H("Mac: 0x%02x%02x%02x%02x%02x%02x \n",
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
			memcpy(evt_data_all->mac_addr, mac, sizeof(evt_data_all->mac_addr));
		}
		if(iface_name)
		{
			IPACMDBG_H("Iface: %s\n", iface_name);
			memcpy(evt_data_all->iface_name, iface_name, sizeof(evt_data_all->iface_name));
		}
		eth_bridge_evt.evt_data = (void*)evt_data_all;
	}
	else
	{
		evt_data_eth_bridge = (ipacm_event_eth_bridge*)malloc(sizeof(*evt_data_eth_bridge));
		if(evt_data_eth_bridge == NULL)
		{
			IPACMERR("Failed to allocate memory.\n");
			return;
		}
		memset(evt_data_eth_bridge, 0, sizeof(*evt_data_eth_bridge));

		evt_data_eth_bridge->p_iface = this;
		evt_data_eth_bridge->iptype = iptype;
		if(mac)
		{
			IPACMDBG_H("Mac: 0x%02x%02x%02x%02x%02x%02x \n",
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
			memcpy(evt_data_eth_bridge->mac_addr, mac, sizeof(evt_data_eth_bridge->mac_addr));
		}
		if(iface_name)
		{
			IPACMDBG_H("Iface: %s\n", iface_name);
			memcpy(evt_data_eth_bridge->iface_name, iface_name,
				sizeof(evt_data_eth_bridge->iface_name));
		}
		eth_bridge_evt.evt_data = (void*)evt_data_eth_bridge;
	}

	IPACMDBG_H("Posting event %s\n",
		IPACM_Iface::ipacmcfg->getEventName(evt));
	IPACM_EvtDispatcher::PostEvt(&eth_bridge_evt);
}

/* add header processing context and return handle to lan2lan controller */
int IPACM_Lan::eth_bridge_add_hdr_proc_ctx(ipa_hdr_l2_type peer_l2_hdr_type, uint32_t *hdl)
{
	int len, res = IPACM_SUCCESS;
	uint32_t hdr_template;
	ipa_ioc_add_hdr_proc_ctx* pHeaderProcTable = NULL;

	if(tx_prop == NULL)
	{
		IPACMERR("No tx prop.\n");
		return IPACM_FAILURE;
	}

	len = sizeof(struct ipa_ioc_add_hdr_proc_ctx) + sizeof(struct ipa_hdr_proc_ctx_add);
	pHeaderProcTable = (ipa_ioc_add_hdr_proc_ctx*)malloc(len);
	if(pHeaderProcTable == NULL)
	{
		IPACMERR("Cannot allocate header processing context table.\n");
		return IPACM_FAILURE;
	}

	memset(pHeaderProcTable, 0, len);
	pHeaderProcTable->commit = 1;
	pHeaderProcTable->num_proc_ctxs = 1;
	pHeaderProcTable->proc_ctx[0].type = eth_bridge_get_hdr_proc_type(peer_l2_hdr_type, tx_prop->tx[0].hdr_l2_type);
	eth_bridge_get_hdr_template_hdl(&hdr_template);
	pHeaderProcTable->proc_ctx[0].hdr_hdl = hdr_template;
	if (m_header.AddHeaderProcCtx(pHeaderProcTable) == false)
	{
		IPACMERR("Adding hdr proc ctx failed with status: %d\n", pHeaderProcTable->proc_ctx[0].status);
		res = IPACM_FAILURE;
		goto end;
	}

	*hdl = pHeaderProcTable->proc_ctx[0].proc_ctx_hdl;

end:
	free(pHeaderProcTable);
	return res;
}

/* add routing rule and return handle to lan2lan controller */
int IPACM_Lan::eth_bridge_add_rt_rule(uint8_t *mac, char *rt_tbl_name, uint32_t hdr_proc_ctx_hdl,
		ipa_hdr_l2_type peer_l2_hdr_type, ipa_ip_type iptype, uint32_t *rt_rule_hdl, int *rt_rule_count)
{
	int i, len, res = IPACM_SUCCESS;
	struct ipa_ioc_add_rt_rule* rt_rule_table = NULL;
	struct ipa_rt_rule_add rt_rule;
	int position, num_rt_rule;

	IPACMDBG_H("Received client MAC 0x%02x%02x%02x%02x%02x%02x.\n",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	num_rt_rule = each_client_rt_rule_count[iptype];

	len = sizeof(ipa_ioc_add_rt_rule) + num_rt_rule * sizeof(ipa_rt_rule_add);
	rt_rule_table = (ipa_ioc_add_rt_rule*)malloc(len);
	if (rt_rule_table == NULL)
	{
		IPACMERR("Failed to allocate memory.\n");
		return IPACM_FAILURE;
	}
	memset(rt_rule_table, 0, len);

	rt_rule_table->commit = 1;
	rt_rule_table->ip = iptype;
	rt_rule_table->num_rules = num_rt_rule;
	strlcpy(rt_rule_table->rt_tbl_name, rt_tbl_name, sizeof(rt_rule_table->rt_tbl_name));
	rt_rule_table->rt_tbl_name[IPA_RESOURCE_NAME_MAX-1] = 0;

	memset(&rt_rule, 0, sizeof(ipa_rt_rule_add));
	rt_rule.at_rear = false;
	rt_rule.status = -1;
	rt_rule.rt_rule_hdl = -1;
#ifdef FEATURE_IPA_V3
	rt_rule.rule.hashable = true;
#endif
	rt_rule.rule.hdr_hdl = 0;
	rt_rule.rule.hdr_proc_ctx_hdl = hdr_proc_ctx_hdl;

	position = 0;
	for(i=0; i<iface_query->num_tx_props; i++)
	{
		if(tx_prop->tx[i].ip == iptype)
		{
			if(position >= num_rt_rule || position >= MAX_NUM_PROP)
			{
				IPACMERR("Number of routing rules already exceeds limit.\n");
				res = IPACM_FAILURE;
				goto end;
			}

			if(ipa_if_cate == WLAN_IF && IPACM_Iface::ipacmcfg->isMCC_Mode)
			{
				IPACMDBG_H("In WLAN MCC mode, use alt dst pipe: %d\n",
						tx_prop->tx[i].alt_dst_pipe);
				rt_rule.rule.dst = tx_prop->tx[i].alt_dst_pipe;
			}
			else
			{
				IPACMDBG_H("It is not WLAN MCC mode, use dst pipe: %d\n",
						tx_prop->tx[i].dst_pipe);
				rt_rule.rule.dst = tx_prop->tx[i].dst_pipe;
			}

			memcpy(&rt_rule.rule.attrib, &tx_prop->tx[i].attrib, sizeof(rt_rule.rule.attrib));
			if(peer_l2_hdr_type == IPA_HDR_L2_ETHERNET_II)
				rt_rule.rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_ETHER_II;
			else
				rt_rule.rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_802_3;
			memcpy(rt_rule.rule.attrib.dst_mac_addr, mac, sizeof(rt_rule.rule.attrib.dst_mac_addr));
			memset(rt_rule.rule.attrib.dst_mac_addr_mask, 0xFF, sizeof(rt_rule.rule.attrib.dst_mac_addr_mask));

			memcpy(&(rt_rule_table->rules[position]), &rt_rule, sizeof(rt_rule_table->rules[position]));
			position++;
		}
	}
	if(false == m_routing.AddRoutingRule(rt_rule_table))
	{
		IPACMERR("Routing rule addition failed!\n");
		res = IPACM_FAILURE;
		goto end;
	}
	else
	{
		*rt_rule_count = position;
		for(i=0; i<position; i++)
			rt_rule_hdl[i] = rt_rule_table->rules[i].rt_rule_hdl;
	}

end:
	free(rt_rule_table);
	return res;
}

/* modify routing rule*/
int IPACM_Lan::eth_bridge_modify_rt_rule(uint8_t *mac, uint32_t hdr_proc_ctx_hdl,
		ipa_hdr_l2_type peer_l2_hdr_type, ipa_ip_type iptype, uint32_t *rt_rule_hdl, int rt_rule_count)
{
	struct ipa_ioc_mdfy_rt_rule *rt_rule = NULL;
	struct ipa_rt_rule_mdfy *rt_rule_entry;
	int len, index, res = IPACM_SUCCESS;

	if(tx_prop == NULL)
	{
		IPACMDBG_H("No tx properties \n");
		return IPACM_FAILURE;
	}

	if(ipa_if_cate != WLAN_IF)
	{
		IPACMDBG_H("This is not WLAN IF, no need to modify rt rule.\n");
		return IPACM_SUCCESS;
	}

	IPACMDBG_H("Receive WLAN client MAC 0x%02x%02x%02x%02x%02x%02x.\n",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	len = sizeof(struct ipa_ioc_mdfy_rt_rule) + rt_rule_count * sizeof(struct ipa_rt_rule_mdfy);
	rt_rule = (struct ipa_ioc_mdfy_rt_rule *)malloc(len);
	if(rt_rule == NULL)
	{
		IPACMERR("Unable to allocate memory for modify rt rule\n");
		return IPACM_FAILURE;
	}
	memset(rt_rule, 0, len);

	rt_rule->commit = 1;
	rt_rule->num_rules = 0;
	rt_rule->ip = iptype;

	for (index = 0; index < tx_prop->num_tx_props; index++)
	{
		if (tx_prop->tx[index].ip == iptype)
		{
			if (rt_rule->num_rules >= rt_rule_count ||
				rt_rule->num_rules >= MAX_NUM_PROP)
			{
				IPACMERR("Number of routing rules exceeds limit.\n");
				res = IPACM_FAILURE;
				goto end;
			}

			rt_rule_entry = &rt_rule->rules[rt_rule->num_rules];

			if (IPACM_Iface::ipacmcfg->isMCC_Mode)
			{
				IPACMDBG_H("In WLAN MCC mode, use alt dst pipe: %d\n",
						tx_prop->tx[index].alt_dst_pipe);
				rt_rule_entry->rule.dst = tx_prop->tx[index].alt_dst_pipe;
			}
			else
			{
				IPACMDBG_H("In WLAN SCC mode, use dst pipe: %d\n",
						tx_prop->tx[index].dst_pipe);
				rt_rule_entry->rule.dst = tx_prop->tx[index].dst_pipe;
			}

			rt_rule_entry->rule.hdr_hdl = 0;
			rt_rule_entry->rule.hdr_proc_ctx_hdl = hdr_proc_ctx_hdl;
#ifdef FEATURE_IPA_V3
			rt_rule_entry->rule.hashable = true;
#endif
			memcpy(&rt_rule_entry->rule.attrib, &tx_prop->tx[index].attrib,
					sizeof(rt_rule_entry->rule.attrib));
			if(peer_l2_hdr_type == IPA_HDR_L2_ETHERNET_II)
				rt_rule_entry->rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_ETHER_II;
			else
				rt_rule_entry->rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_802_3;
			memcpy(rt_rule_entry->rule.attrib.dst_mac_addr, mac,
					sizeof(rt_rule_entry->rule.attrib.dst_mac_addr));
			memset(rt_rule_entry->rule.attrib.dst_mac_addr_mask, 0xFF,
					sizeof(rt_rule_entry->rule.attrib.dst_mac_addr_mask));

			rt_rule_entry->rt_rule_hdl = rt_rule_hdl[rt_rule->num_rules];
			rt_rule->num_rules++;
		}
	}

	if(m_routing.ModifyRoutingRule(rt_rule) == false)
	{
		IPACMERR("Failed to modify routing rules.\n");
		res = IPACM_FAILURE;
		goto end;
	}
	if(m_routing.Commit(iptype) == false)
	{
		IPACMERR("Failed to commit routing rules.\n");
		res = IPACM_FAILURE;
		goto end;
	}
	IPACMDBG("Modified routing rules successfully.\n");

end:
	free(rt_rule);
	return res;
}

int IPACM_Lan::eth_bridge_add_flt_rule(uint8_t *mac, uint32_t rt_tbl_hdl, ipa_ip_type iptype, uint32_t *flt_rule_hdl)
{
	int len, res = IPACM_SUCCESS;
	struct ipa_flt_rule_add flt_rule_entry;
	struct ipa_ioc_add_flt_rule_after *pFilteringTable = NULL;

#ifdef FEATURE_IPA_V3
	if (rx_prop == NULL || tx_prop == NULL)
	{
		IPACMDBG_H("No rx or tx properties registered for iface %s\n", dev_name);
		return IPACM_FAILURE;
	}

	IPACMDBG_H("Received client MAC 0x%02x%02x%02x%02x%02x%02x.\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	len = sizeof(struct ipa_ioc_add_flt_rule_after) + sizeof(struct ipa_flt_rule_add);
	pFilteringTable = (struct ipa_ioc_add_flt_rule_after*)malloc(len);
	if (!pFilteringTable)
	{
		IPACMERR("Failed to allocate ipa_ioc_add_flt_rule_after memory...\n");
		return IPACM_FAILURE;
	}
	memset(pFilteringTable, 0, len);

	/* add mac based rule*/
	pFilteringTable->commit = 1;
	pFilteringTable->ep = rx_prop->rx[0].src_pipe;
	pFilteringTable->ip = iptype;
	pFilteringTable->num_rules = 1;
	pFilteringTable->add_after_hdl = eth_bridge_flt_rule_offset[iptype];

	memset(&flt_rule_entry, 0, sizeof(flt_rule_entry));
	flt_rule_entry.at_rear = 1;

	flt_rule_entry.rule.retain_hdr = 0;
	flt_rule_entry.rule.to_uc = 0;
	flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
	flt_rule_entry.rule.eq_attrib_type = 0;
	flt_rule_entry.rule.rt_tbl_hdl = rt_tbl_hdl;
	flt_rule_entry.rule.hashable = true;

	memcpy(&flt_rule_entry.rule.attrib, &rx_prop->rx[0].attrib, sizeof(flt_rule_entry.rule.attrib));
	if(tx_prop->tx[0].hdr_l2_type == IPA_HDR_L2_ETHERNET_II)
	{
		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_ETHER_II;
	}
	else
	{
		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_802_3;
	}

	memcpy(flt_rule_entry.rule.attrib.dst_mac_addr, mac, sizeof(flt_rule_entry.rule.attrib.dst_mac_addr));
	memset(flt_rule_entry.rule.attrib.dst_mac_addr_mask, 0xFF, sizeof(flt_rule_entry.rule.attrib.dst_mac_addr_mask));

	memcpy(&(pFilteringTable->rules[0]), &flt_rule_entry, sizeof(flt_rule_entry));
	if (false == m_filtering.AddFilteringRuleAfter(pFilteringTable))
	{
		IPACMERR("Failed to add client filtering rules.\n");
		res = IPACM_FAILURE;
		goto end;
	}
	*flt_rule_hdl = pFilteringTable->rules[0].flt_rule_hdl;

end:
	free(pFilteringTable);
#endif
	return res;
}

int IPACM_Lan::eth_bridge_del_flt_rule(uint32_t flt_rule_hdl, ipa_ip_type iptype)
{
	if(m_filtering.DeleteFilteringHdls(&flt_rule_hdl, iptype, 1) == false)
	{
		IPACMERR("Failed to delete the client specific flt rule.\n");
		return IPACM_FAILURE;
	}
	return IPACM_SUCCESS;
}

int IPACM_Lan::eth_bridge_del_rt_rule(uint32_t rt_rule_hdl, ipa_ip_type iptype)
{
	if(m_routing.DeleteRoutingHdl(rt_rule_hdl, iptype) == false)
	{
		IPACMERR("Failed to delete routing rule.\n");
		return IPACM_FAILURE;
	}
	return IPACM_SUCCESS;
}

/* delete header processing context */
int IPACM_Lan::eth_bridge_del_hdr_proc_ctx(uint32_t hdr_proc_ctx_hdl)
{
	if(m_header.DeleteHeaderProcCtx(hdr_proc_ctx_hdl) == false)
	{
		IPACMERR("Failed to delete hdr proc ctx.\n");
		return IPACM_FAILURE;
	}
	return IPACM_SUCCESS;
}

/* check if the event is associated with vlan interface */
bool IPACM_Lan::is_vlan_event(char *event_iface_name)
{
	int self_name_len, event_iface_name_len;
	if(event_iface_name == NULL)
	{
		IPACMERR("Invalid input\n");
		return false;
	}

	IPACMDBG_H("Self iface %s, event iface %s\n", dev_name, event_iface_name);
	self_name_len = strlen(dev_name);
	event_iface_name_len = strlen(event_iface_name);

	if(event_iface_name_len > self_name_len && strncmp(dev_name, event_iface_name, self_name_len) == 0)
	{
		IPACMDBG_H("This is vlan event.\n");
		return true;
	}
	return false;
}

/* check if the event is associated with l2tp interface */
bool IPACM_Lan::is_l2tp_event(char *event_iface_name)
{
	if(event_iface_name == NULL)
	{
		IPACMERR("Invalid input\n");
		return false;
	}

	IPACMDBG_H("Self iface %s, event iface %s\n", dev_name, event_iface_name);
	if(strncmp(event_iface_name, "l2tp", 4) == 0)
	{
		IPACMDBG_H("This is l2tp event.\n");
		return true;
	}
	return false;
}

/* add l2tp rt rule for l2tp client */
int IPACM_Lan::add_l2tp_rt_rule(ipa_ip_type iptype, uint8_t *dst_mac, ipa_hdr_l2_type peer_l2_hdr_type,
	uint32_t l2tp_session_id, uint32_t vlan_id, uint8_t *vlan_client_mac, uint32_t *vlan_iface_ipv6_addr,
	uint32_t *vlan_client_ipv6_addr, uint32_t *first_pass_hdr_hdl, uint32_t *first_pass_hdr_proc_ctx_hdl,
	uint32_t *second_pass_hdr_hdl, int *num_rt_hdl, uint32_t *first_pass_rt_rule_hdl, uint32_t *second_pass_rt_rule_hdl)
{
	int i, size, position;
	uint32_t vlan_iface_ipv6_addr_network[4], vlan_client_ipv6_addr_network[4];
	ipa_ioc_add_hdr *hdr_table;
	ipa_hdr_add *hdr;
	ipa_ioc_add_hdr_proc_ctx *hdr_proc_ctx_table;
	ipa_hdr_proc_ctx_add *hdr_proc_ctx;
	ipa_ioc_add_rt_rule* rt_rule_table;
	ipa_rt_rule_add *rt_rule;
	ipa_ioc_copy_hdr copy_hdr;

	if(tx_prop == NULL)
	{
		IPACMERR("No tx prop.\n");
		return IPACM_FAILURE;
	}

	/* =========== install first pass hdr template (IPv6 + L2TP + inner ETH header = 62 bytes) ============= */
	if(*first_pass_hdr_hdl != 0)
	{
		IPACMDBG_H("First pass hdr template was added before.\n");
	}
	else
	{
		size = sizeof(ipa_ioc_add_hdr) + sizeof(ipa_hdr_add);
		hdr_table = (ipa_ioc_add_hdr*)malloc(size);
		if(hdr_table == NULL)
		{
			IPACMERR("Failed to allocate memory.\n");
			return IPACM_FAILURE;
		}
		memset(hdr_table, 0, size);

		hdr_table->commit = 1;
		hdr_table->num_hdrs = 1;
		hdr = &hdr_table->hdr[0];

		if(iptype == IPA_IP_v4)
		{
			snprintf(hdr->name, sizeof(hdr->name), "vlan_%d_l2tp_%d_v4", vlan_id, l2tp_session_id);
		}
		else
		{
			snprintf(hdr->name, sizeof(hdr->name), "vlan_%d_l2tp_%d_v6", vlan_id, l2tp_session_id);
		}
		hdr->hdr_len = 62;
		hdr->type = IPA_HDR_L2_ETHERNET_II;
		hdr->is_partial = 0;

		hdr->hdr[0] = 0x60;	/* version */
		hdr->hdr[6] = 0x73; /* next header = L2TP */
		hdr->hdr[7] = 0x40; /* hop limit = 64 */
		for(i = 0; i < 4; i++)
		{
			vlan_iface_ipv6_addr_network[i] = htonl(vlan_iface_ipv6_addr[i]);
			vlan_client_ipv6_addr_network[i] = htonl(vlan_client_ipv6_addr[i]);
		}
		memcpy(hdr->hdr + 8, vlan_iface_ipv6_addr_network, 16); /* source IPv6 addr */
		memcpy(hdr->hdr + 24, vlan_client_ipv6_addr_network, 16); /* dest IPv6 addr */
		hdr->hdr[43] = (uint8_t)(l2tp_session_id & 0xFF); /* l2tp header */
		hdr->hdr[42] = (uint8_t)(l2tp_session_id >> 8 & 0xFF);
		hdr->hdr[41] = (uint8_t)(l2tp_session_id >> 16 & 0xFF);
		hdr->hdr[40] = (uint8_t)(l2tp_session_id >> 24 & 0xFF);

		if(m_header.AddHeader(hdr_table) == false)
		{
			IPACMERR("Failed to add hdr with status: %d\n", hdr_table->hdr[0].status);
			free(hdr_table);
			return IPACM_FAILURE;
		}
		*first_pass_hdr_hdl = hdr_table->hdr[0].hdr_hdl;
		IPACMDBG_H("Installed first pass hdr: hdl %d\n", *first_pass_hdr_hdl);
		free(hdr_table);
	}

	/* =========== install first pass hdr proc ctx (populate src/dst MAC and Ether type) ============= */
	size = sizeof(ipa_ioc_add_hdr_proc_ctx) + sizeof(ipa_hdr_proc_ctx_add);
	hdr_proc_ctx_table = (ipa_ioc_add_hdr_proc_ctx*)malloc(size);
	if(hdr_proc_ctx_table == NULL)
	{
		IPACMERR("Failed to allocate memory.\n");
		return IPACM_FAILURE;
	}
	memset(hdr_proc_ctx_table, 0, size);

	hdr_proc_ctx_table->commit = 1;
	hdr_proc_ctx_table->num_proc_ctxs = 1;
	hdr_proc_ctx = &hdr_proc_ctx_table->proc_ctx[0];

	hdr_proc_ctx->type = IPA_HDR_PROC_L2TP_HEADER_ADD;
	hdr_proc_ctx->hdr_hdl = *first_pass_hdr_hdl;
	hdr_proc_ctx->l2tp_params.hdr_add_param.eth_hdr_retained = 1;
	hdr_proc_ctx->l2tp_params.hdr_add_param.input_ip_version = iptype;
	hdr_proc_ctx->l2tp_params.hdr_add_param.output_ip_version = IPA_IP_v6;
	if(m_header.AddHeaderProcCtx(hdr_proc_ctx_table) == false)
	{
		IPACMERR("Failed to add hdr proc ctx with status: %d\n", hdr_proc_ctx_table->proc_ctx[0].status);
		free(hdr_proc_ctx_table);
		return IPACM_FAILURE;
	}
	*first_pass_hdr_proc_ctx_hdl = hdr_proc_ctx_table->proc_ctx[0].proc_ctx_hdl;
	IPACMDBG_H("Installed first pass hdr proc ctx: hdl %d\n", *first_pass_hdr_proc_ctx_hdl);
	free(hdr_proc_ctx_table);

	/* =========== install first pass rt rules (match dst MAC then doing UCP) ============= */
	*num_rt_hdl = each_client_rt_rule_count[iptype];
	size = sizeof(ipa_ioc_add_rt_rule) + (*num_rt_hdl) * sizeof(ipa_rt_rule_add);
	rt_rule_table = (ipa_ioc_add_rt_rule*)malloc(size);
	if (rt_rule_table == NULL)
	{
		IPACMERR("Failed to allocate memory.\n");
		return IPACM_FAILURE;
	}
	memset(rt_rule_table, 0, size);

	rt_rule_table->commit = 1;
	rt_rule_table->ip = iptype;
	rt_rule_table->num_rules = *num_rt_hdl;
	snprintf(rt_rule_table->rt_tbl_name, sizeof(rt_rule_table->rt_tbl_name), "l2tp");
	rt_rule_table->rt_tbl_name[IPA_RESOURCE_NAME_MAX-1] = 0;

	position = 0;
	for(i = 0; i < iface_query->num_tx_props; i++)
	{
		if(tx_prop->tx[i].ip == iptype)
		{
			if(position >= *num_rt_hdl || position >= MAX_NUM_PROP)
			{
				IPACMERR("Number of routing rules already exceeds limit.\n");
				free(rt_rule_table);
				return IPACM_FAILURE;
			}

			rt_rule = &rt_rule_table->rules[position];
			rt_rule->at_rear = false;
			rt_rule->status = -1;
			rt_rule->rt_rule_hdl = -1;
			rt_rule->rule.hashable = false;	//WLAN->ETH direction rules are set to non-hashable to keep consistent with the other direction
			rt_rule->rule.hdr_hdl = 0;
			rt_rule->rule.hdr_proc_ctx_hdl = *first_pass_hdr_proc_ctx_hdl;
			rt_rule->rule.dst = IPA_CLIENT_DUMMY_CONS;

			memcpy(&rt_rule->rule.attrib, &tx_prop->tx[i].attrib, sizeof(rt_rule->rule.attrib));
			if(peer_l2_hdr_type == IPA_HDR_L2_ETHERNET_II)
				rt_rule->rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_ETHER_II;
			else
				rt_rule->rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_802_3;
			memcpy(rt_rule->rule.attrib.dst_mac_addr, dst_mac, sizeof(rt_rule->rule.attrib.dst_mac_addr));
			memset(rt_rule->rule.attrib.dst_mac_addr_mask, 0xFF, sizeof(rt_rule->rule.attrib.dst_mac_addr_mask));
			position++;
		}
	}
	if(m_routing.AddRoutingRule(rt_rule_table) == false)
	{
		IPACMERR("Failed to add first pass rt rules.\n");
		free(rt_rule_table);
		return IPACM_FAILURE;
	}
	for(i = 0; i < position; i++)
	{
		first_pass_rt_rule_hdl[i] = rt_rule_table->rules[i].rt_rule_hdl;
	}
	free(rt_rule_table);

	/* =========== install second pass hdr (Ethernet header with L2TP tag = 18 bytes) ============= */
	if(*second_pass_hdr_hdl != 0)
	{
		IPACMDBG_H("Second pass hdr was added before.\n");
	}
	else
	{
		size = sizeof(ipa_ioc_add_hdr) + sizeof(ipa_hdr_add);
		hdr_table = (ipa_ioc_add_hdr*)malloc(size);
		if(hdr_table == NULL)
		{
			IPACMERR("Failed to allocate memory.\n");
			return IPACM_FAILURE;
		}
		memset(hdr_table, 0, size);

		hdr_table->commit = 1;
		hdr_table->num_hdrs = 1;
		hdr = &hdr_table->hdr[0];

		if(iptype == IPA_IP_v4)
		{
			snprintf(hdr->name, sizeof(hdr->name), "vlan_%d_v4", vlan_id);
		}
		else
		{
			snprintf(hdr->name, sizeof(hdr->name), "vlan_%d_v6", vlan_id);
		}
		hdr->type = IPA_HDR_L2_ETHERNET_II;
		hdr->is_partial = 0;
		for(i = 0; i < tx_prop->num_tx_props; i++)
		{
			if(tx_prop->tx[i].ip == IPA_IP_v6)
			{
				memset(&copy_hdr, 0, sizeof(copy_hdr));
				strlcpy(copy_hdr.name, tx_prop->tx[i].hdr_name,
					sizeof(copy_hdr.name));
				IPACMDBG_H("Header name: %s in tx:%d\n", copy_hdr.name, i);
				if(m_header.CopyHeader(&copy_hdr) == false)
				{
					IPACMERR("Failed to get partial header.\n");
					free(hdr_table);
					return IPACM_FAILURE;
				}
				IPACMDBG_H("Header length: %d\n", copy_hdr.hdr_len);
				hdr->hdr_len = copy_hdr.hdr_len;
				memcpy(hdr->hdr, copy_hdr.hdr, hdr->hdr_len);
				break;
			}
		}
		/* copy vlan client mac */
		memcpy(hdr->hdr + hdr->hdr_len - 18, vlan_client_mac, 6);
		hdr->hdr[hdr->hdr_len - 3] = (uint8_t)vlan_id & 0xFF;
		hdr->hdr[hdr->hdr_len - 4] = (uint8_t)(vlan_id >> 8) & 0xFF;

		if(m_header.AddHeader(hdr_table) == false)
		{
			IPACMERR("Failed to add hdr with status: %d\n", hdr->status);
			free(hdr_table);
			return IPACM_FAILURE;
		}
		*second_pass_hdr_hdl = hdr->hdr_hdl;
		IPACMDBG_H("Installed second pass hdr: hdl %d\n", *second_pass_hdr_hdl);
		free(hdr_table);
	}

	/* =========== install second pass rt rules (match VLAN interface IPv6 address at dst client side) ============= */
	if(second_pass_rt_rule_hdl[0] != 0)
	{
		IPACMDBG_H("Second pass rt rule was added before, return.\n");
		return IPACM_SUCCESS;
	}

	*num_rt_hdl = each_client_rt_rule_count[IPA_IP_v6];
	size = sizeof(ipa_ioc_add_rt_rule) + (*num_rt_hdl) * sizeof(ipa_rt_rule_add);
	rt_rule_table = (ipa_ioc_add_rt_rule*)malloc(size);
	if (rt_rule_table == NULL)
	{
		IPACMERR("Failed to allocate memory.\n");
		return IPACM_FAILURE;
	}
	memset(rt_rule_table, 0, size);

	rt_rule_table->commit = 1;
	rt_rule_table->ip = IPA_IP_v6;
	rt_rule_table->num_rules = *num_rt_hdl;
	snprintf(rt_rule_table->rt_tbl_name, sizeof(rt_rule_table->rt_tbl_name), "l2tp");
	rt_rule_table->rt_tbl_name[IPA_RESOURCE_NAME_MAX-1] = 0;

	position = 0;
	for(i = 0; i < iface_query->num_tx_props; i++)
	{
		if(tx_prop->tx[i].ip == IPA_IP_v6)
		{
			if(position >= *num_rt_hdl || position >= MAX_NUM_PROP)
			{
				IPACMERR("Number of routing rules already exceeds limit.\n");
				free(rt_rule_table);
				return IPACM_FAILURE;
			}

			rt_rule = &rt_rule_table->rules[position];
			rt_rule->at_rear = false;
			rt_rule->status = -1;
			rt_rule->rt_rule_hdl = -1;
			rt_rule->rule.hashable = false;	//WLAN->ETH direction rules are set to non-hashable to keep consistent with the other direction
			rt_rule->rule.hdr_hdl = *second_pass_hdr_hdl;
			rt_rule->rule.hdr_proc_ctx_hdl = 0;
			rt_rule->rule.dst = tx_prop->tx[i].dst_pipe;

			memcpy(&rt_rule->rule.attrib, &tx_prop->tx[i].attrib, sizeof(rt_rule->rule.attrib));
			rt_rule->rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
			memcpy(rt_rule->rule.attrib.u.v6.dst_addr, vlan_client_ipv6_addr,
				sizeof(rt_rule->rule.attrib.u.v6.dst_addr));
			memset(rt_rule->rule.attrib.u.v6.dst_addr_mask, 0xFF, sizeof(rt_rule->rule.attrib.u.v6.dst_addr_mask));
			position++;
		}
	}
	if(m_routing.AddRoutingRule(rt_rule_table) == false)
	{
		IPACMERR("Failed to add second pass rt rules.\n");
		free(rt_rule_table);
		return IPACM_FAILURE;
	}
	for(i = 0; i < position; i++)
	{
		second_pass_rt_rule_hdl[i] = rt_rule_table->rules[i].rt_rule_hdl;
	}
	free(rt_rule_table);

	return IPACM_SUCCESS;
}

/* delete l2tp rt rule for l2tp client */
int IPACM_Lan::del_l2tp_rt_rule(ipa_ip_type iptype, uint32_t first_pass_hdr_hdl, uint32_t first_pass_hdr_proc_ctx_hdl,
	uint32_t second_pass_hdr_hdl, int num_rt_hdl, uint32_t *first_pass_rt_rule_hdl, uint32_t *second_pass_rt_rule_hdl)
{
	int i;

	if(num_rt_hdl < 0)
	{
		IPACMERR("Invalid num rt rule: %d\n", num_rt_hdl);
		return IPACM_FAILURE;
	}

	for(i = 0; i < num_rt_hdl; i++)
	{
		if(first_pass_rt_rule_hdl != NULL)
		{
			if(m_routing.DeleteRoutingHdl(first_pass_rt_rule_hdl[i], iptype) == false)
			{
				return IPACM_FAILURE;
			}
		}
		if(second_pass_rt_rule_hdl != NULL)
		{
			if(m_routing.DeleteRoutingHdl(second_pass_rt_rule_hdl[i], IPA_IP_v6) == false)
			{
				return IPACM_FAILURE;
			}
		}
	}

	if(first_pass_hdr_proc_ctx_hdl != 0)
	{
		if(m_header.DeleteHeaderProcCtx(first_pass_hdr_proc_ctx_hdl) == false)
		{
			return IPACM_FAILURE;
		}
	}

	if(first_pass_hdr_hdl != 0)
	{
		if(m_header.DeleteHeaderHdl(first_pass_hdr_hdl) == false)
		{
			return IPACM_FAILURE;
		}
	}
	if(second_pass_hdr_hdl != 0)
	{
		if(m_header.DeleteHeaderHdl(second_pass_hdr_hdl) == false)
		{
			return IPACM_FAILURE;
		}
	}

	return IPACM_SUCCESS;
}

/* add l2tp rt rule for non l2tp client */
int IPACM_Lan::add_l2tp_rt_rule(ipa_ip_type iptype, uint8_t *dst_mac, uint32_t *hdr_proc_ctx_hdl,
	int *num_rt_hdl, uint32_t *rt_rule_hdl)
{
	int i, size, position;
	ipa_ioc_add_hdr_proc_ctx *hdr_proc_ctx_table;
	ipa_hdr_proc_ctx_add *hdr_proc_ctx;
	ipa_ioc_add_rt_rule* rt_rule_table;
	ipa_rt_rule_add *rt_rule;
	ipa_ioc_get_hdr hdr;

	if(tx_prop == NULL)
	{
		IPACMERR("No tx prop.\n");
		return IPACM_FAILURE;
	}

	memset(&hdr, 0, sizeof(hdr));
	for(i = 0; i < tx_prop->num_tx_props; i++)
	{
		if(tx_prop->tx[i].ip == iptype)
		{
			strlcpy(hdr.name, tx_prop->tx[i].hdr_name,
				sizeof(hdr.name));
			break;
		}
	}
	if(m_header.GetHeaderHandle(&hdr) == false)
	{
		IPACMERR("Failed to get template hdr hdl.\n");
		return IPACM_FAILURE;
	}

	/* =========== install hdr proc ctx (uc needs to remove IPv6 + L2TP + inner ETH header = 62 bytes) ============= */
	if(*hdr_proc_ctx_hdl != 0)
	{
		IPACMDBG_H("Hdr proc ctx was added before.\n");
	}
	else
	{
		size = sizeof(ipa_ioc_add_hdr_proc_ctx) + sizeof(ipa_hdr_proc_ctx_add);
		hdr_proc_ctx_table = (ipa_ioc_add_hdr_proc_ctx*)malloc(size);
		if(hdr_proc_ctx_table == NULL)
		{
			IPACMERR("Failed to allocate memory.\n");
			return IPACM_FAILURE;
		}
		memset(hdr_proc_ctx_table, 0, size);

		hdr_proc_ctx_table->commit = 1;
		hdr_proc_ctx_table->num_proc_ctxs = 1;
		hdr_proc_ctx = &hdr_proc_ctx_table->proc_ctx[0];

		hdr_proc_ctx->type = IPA_HDR_PROC_L2TP_HEADER_REMOVE;
		hdr_proc_ctx->hdr_hdl = hdr.hdl;
		hdr_proc_ctx->l2tp_params.hdr_remove_param.hdr_len_remove = 62;
		hdr_proc_ctx->l2tp_params.hdr_remove_param.eth_hdr_retained = 1;
		hdr_proc_ctx->l2tp_params.is_dst_pipe_valid = 1;
		hdr_proc_ctx->l2tp_params.dst_pipe = tx_prop->tx[0].dst_pipe;
		IPACMDBG_H("Header_remove: hdr len %d, hdr retained %d, dst client: %d\n",
			hdr_proc_ctx->l2tp_params.hdr_remove_param.hdr_len_remove,
			hdr_proc_ctx->l2tp_params.hdr_remove_param.eth_hdr_retained,
			hdr_proc_ctx->l2tp_params.dst_pipe);
		if(m_header.AddHeaderProcCtx(hdr_proc_ctx_table) == false)
		{
			IPACMERR("Failed to add hdr proc ctx with status: %d\n", hdr_proc_ctx_table->proc_ctx[0].status);
			free(hdr_proc_ctx_table);
			return IPACM_FAILURE;
		}
		*hdr_proc_ctx_hdl = hdr_proc_ctx_table->proc_ctx[0].proc_ctx_hdl;
		IPACMDBG_H("Installed hdr proc ctx: hdl %d\n", *hdr_proc_ctx_hdl);
		free(hdr_proc_ctx_table);
	}

	/* =========== install rt rules (match dst MAC within 62 bytes header) ============= */
	*num_rt_hdl = each_client_rt_rule_count[iptype];
	size = sizeof(ipa_ioc_add_rt_rule) + (*num_rt_hdl) * sizeof(ipa_rt_rule_add);
	rt_rule_table = (ipa_ioc_add_rt_rule*)malloc(size);
	if (rt_rule_table == NULL)
	{
		IPACMERR("Failed to allocate memory.\n");
		return IPACM_FAILURE;
	}
	memset(rt_rule_table, 0, size);

	rt_rule_table->commit = 1;
	rt_rule_table->ip = iptype;
	rt_rule_table->num_rules = *num_rt_hdl;
	snprintf(rt_rule_table->rt_tbl_name, sizeof(rt_rule_table->rt_tbl_name), "l2tp");
	rt_rule_table->rt_tbl_name[IPA_RESOURCE_NAME_MAX-1] = 0;

	position = 0;
	for(i = 0; i < iface_query->num_tx_props; i++)
	{
		if(tx_prop->tx[i].ip == iptype)
		{
			if(position >= *num_rt_hdl || position >= MAX_NUM_PROP)
			{
				IPACMERR("Number of routing rules already exceeds limit.\n");
				free(rt_rule_table);
				return IPACM_FAILURE;
			}

			rt_rule = &rt_rule_table->rules[position];
			rt_rule->at_rear = false;
			rt_rule->status = -1;
			rt_rule->rt_rule_hdl = -1;
			rt_rule->rule.hashable = false;	//ETH->WLAN direction rules need to be non-hashable due to encapsulation

			rt_rule->rule.hdr_hdl = 0;
			rt_rule->rule.hdr_proc_ctx_hdl = *hdr_proc_ctx_hdl;
			rt_rule->rule.dst = tx_prop->tx[i].dst_pipe;

			memcpy(&rt_rule->rule.attrib, &tx_prop->tx[i].attrib, sizeof(rt_rule->rule.attrib));

			rt_rule->rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_L2TP;
			memset(rt_rule->rule.attrib.dst_mac_addr_mask, 0xFF, sizeof(rt_rule->rule.attrib.dst_mac_addr_mask));
			memcpy(rt_rule->rule.attrib.dst_mac_addr, dst_mac, sizeof(rt_rule->rule.attrib.dst_mac_addr));

			position++;
		}
	}
	if(m_routing.AddRoutingRule(rt_rule_table) == false)
	{
		IPACMERR("Failed to add first pass rt rules.\n");
		free(rt_rule_table);
		return IPACM_FAILURE;
	}
	for(i = 0; i < position; i++)
		rt_rule_hdl[i] = rt_rule_table->rules[i].rt_rule_hdl;

	free(rt_rule_table);
	return IPACM_SUCCESS;
}

int IPACM_Lan::del_l2tp_rt_rule(ipa_ip_type iptype, int num_rt_hdl, uint32_t *rt_rule_hdl)
{
	int i;

	if(num_rt_hdl < 0)
	{
		IPACMERR("Invalid num rt rule: %d\n", num_rt_hdl);
		return IPACM_FAILURE;
	}

	for(i = 0; i < num_rt_hdl; i++)
	{
		if(m_routing.DeleteRoutingHdl(rt_rule_hdl[i], iptype) == false)
		{
			return IPACM_FAILURE;
		}
	}

	return IPACM_SUCCESS;
}

/* add l2tp flt rule on l2tp interface */
int IPACM_Lan::add_l2tp_flt_rule(uint8_t *dst_mac, uint32_t *flt_rule_hdl)
{
	int len;
	int fd_ipa;
	struct ipa_flt_rule_add flt_rule_entry;
	struct ipa_ioc_add_flt_rule_after *pFilteringTable = NULL;
	ipa_ioc_get_rt_tbl rt_tbl;

#ifdef FEATURE_IPA_V3
	if (rx_prop == NULL || tx_prop == NULL)
	{
		IPACMDBG_H("No rx or tx properties registered for iface %s\n", dev_name);
		return IPACM_FAILURE;
	}

	len = sizeof(struct ipa_ioc_add_flt_rule_after) + sizeof(struct ipa_flt_rule_add);
	pFilteringTable = (struct ipa_ioc_add_flt_rule_after*)malloc(len);
	if (!pFilteringTable)
	{
		IPACMERR("Failed to allocate ipa_ioc_add_flt_rule_after memory...\n");
		return IPACM_FAILURE;
	}
	memset(pFilteringTable, 0, len);

	pFilteringTable->commit = 1;
	pFilteringTable->ep = rx_prop->rx[0].src_pipe;
	pFilteringTable->ip = IPA_IP_v6;
	pFilteringTable->num_rules = 1;
	pFilteringTable->add_after_hdl = eth_bridge_flt_rule_offset[IPA_IP_v6];

	fd_ipa = open(IPA_DEVICE_NAME, O_RDWR);
	if(fd_ipa == 0)
	{
		IPACMERR("Failed to open %s\n",IPA_DEVICE_NAME);
		free(pFilteringTable);
		return IPACM_FAILURE;
	}

	rt_tbl.ip = IPA_IP_v6;
	snprintf(rt_tbl.name, sizeof(rt_tbl.name), "l2tp");
	rt_tbl.name[IPA_RESOURCE_NAME_MAX-1] = '\0';
	IPACMDBG_H("This flt rule points to rt tbl %s.\n", rt_tbl.name);
	if(m_routing.GetRoutingTable(&rt_tbl) == false)
	{
		IPACMERR("Failed to get routing table from name\n");
		free(pFilteringTable);
		close(fd_ipa);
		return IPACM_FAILURE;
	}

	memset(&flt_rule_entry, 0, sizeof(flt_rule_entry));
	flt_rule_entry.at_rear = 1;

	flt_rule_entry.rule.retain_hdr = 0;
	flt_rule_entry.rule.to_uc = 0;
	flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
	flt_rule_entry.rule.eq_attrib_type = 0;
	flt_rule_entry.rule.rt_tbl_hdl = rt_tbl.hdl;
	flt_rule_entry.rule.hashable = false;	//ETH->WLAN direction rules need to be non-hashable due to encapsulation

	memcpy(&flt_rule_entry.rule.attrib, &rx_prop->rx[0].attrib, sizeof(flt_rule_entry.rule.attrib));

	/* flt rule is matching dst MAC within 62 bytes header */
	flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_L2TP;
	memset(flt_rule_entry.rule.attrib.dst_mac_addr_mask, 0xFF, sizeof(flt_rule_entry.rule.attrib.dst_mac_addr_mask));
	memcpy(flt_rule_entry.rule.attrib.dst_mac_addr, dst_mac, sizeof(flt_rule_entry.rule.attrib.dst_mac_addr));

	memcpy(&(pFilteringTable->rules[0]), &flt_rule_entry, sizeof(flt_rule_entry));
	if(m_filtering.AddFilteringRuleAfter(pFilteringTable) == false)
	{
		IPACMERR("Failed to add client filtering rules.\n");
		free(pFilteringTable);
		close(fd_ipa);
		return IPACM_FAILURE;
	}
	*flt_rule_hdl = pFilteringTable->rules[0].flt_rule_hdl;

	free(pFilteringTable);
	close(fd_ipa);
#endif
	return IPACM_SUCCESS;
}

/* delete l2tp flt rule on l2tp interface */
int IPACM_Lan::del_l2tp_flt_rule(uint32_t flt_rule_hdl)
{
	if(m_filtering.DeleteFilteringHdls(&flt_rule_hdl, IPA_IP_v6, 1) == false)
	{
		return IPACM_FAILURE;
	}

	return IPACM_SUCCESS;
}

/* add l2tp flt rule on non l2tp interface */
int IPACM_Lan::add_l2tp_flt_rule(ipa_ip_type iptype, uint8_t *dst_mac, uint32_t *vlan_client_ipv6_addr,
	uint32_t *first_pass_flt_rule_hdl, uint32_t *second_pass_flt_rule_hdl)
{
	int len;
	struct ipa_flt_rule_add flt_rule_entry;
	struct ipa_ioc_add_flt_rule_after *pFilteringTable = NULL;
	ipa_ioc_get_rt_tbl rt_tbl;

#ifdef FEATURE_IPA_V3
	if (rx_prop == NULL || tx_prop == NULL)
	{
		IPACMDBG_H("No rx or tx properties registered for iface %s\n", dev_name);
		return IPACM_FAILURE;
	}

	IPACMDBG_H("Dst client MAC 0x%02x%02x%02x%02x%02x%02x.\n", dst_mac[0], dst_mac[1],
		dst_mac[2], dst_mac[3], dst_mac[4], dst_mac[5]);

	len = sizeof(struct ipa_ioc_add_flt_rule_after) + sizeof(struct ipa_flt_rule_add);
	pFilteringTable = (struct ipa_ioc_add_flt_rule_after*)malloc(len);
	if (!pFilteringTable)
	{
		IPACMERR("Failed to allocate ipa_ioc_add_flt_rule_after memory...\n");
		return IPACM_FAILURE;
	}
	memset(pFilteringTable, 0, len);

	pFilteringTable->commit = 1;
	pFilteringTable->ep = rx_prop->rx[0].src_pipe;
	pFilteringTable->ip = iptype;
	pFilteringTable->num_rules = 1;
	pFilteringTable->add_after_hdl = eth_bridge_flt_rule_offset[iptype];

	/* =========== add first pass flt rule (match dst MAC) ============= */
	rt_tbl.ip = iptype;
	snprintf(rt_tbl.name, sizeof(rt_tbl.name), "l2tp");
	IPACMDBG_H("This flt rule points to rt tbl %s.\n", rt_tbl.name);

	if(m_routing.GetRoutingTable(&rt_tbl) == false)
	{
		IPACMERR("Failed to get routing table.\n");
		return IPACM_FAILURE;
	}

	memset(&flt_rule_entry, 0, sizeof(flt_rule_entry));
	flt_rule_entry.at_rear = 1;

	flt_rule_entry.rule.retain_hdr = 0;
	flt_rule_entry.rule.to_uc = 0;
	flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
	flt_rule_entry.rule.eq_attrib_type = 0;
	flt_rule_entry.rule.rt_tbl_hdl = rt_tbl.hdl;
	flt_rule_entry.rule.hashable = false;	//WLAN->ETH direction rules are set to non-hashable to keep consistent with the other direction

	memcpy(&flt_rule_entry.rule.attrib, &rx_prop->rx[0].attrib, sizeof(flt_rule_entry.rule.attrib));
	if(tx_prop->tx[0].hdr_l2_type == IPA_HDR_L2_ETHERNET_II)
	{
		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_ETHER_II;
	}
	else
	{
		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_MAC_DST_ADDR_802_3;
	}

	memcpy(flt_rule_entry.rule.attrib.dst_mac_addr, dst_mac, sizeof(flt_rule_entry.rule.attrib.dst_mac_addr));
	memset(flt_rule_entry.rule.attrib.dst_mac_addr_mask, 0xFF, sizeof(flt_rule_entry.rule.attrib.dst_mac_addr_mask));

	memcpy(&(pFilteringTable->rules[0]), &flt_rule_entry, sizeof(flt_rule_entry));
	if (false == m_filtering.AddFilteringRuleAfter(pFilteringTable))
	{
		IPACMERR("Failed to add first pass filtering rules.\n");
		free(pFilteringTable);
		return IPACM_FAILURE;
	}
	*first_pass_flt_rule_hdl = pFilteringTable->rules[0].flt_rule_hdl;

	/* =========== add second pass flt rule (match VLAN interface IPv6 address at client side) ============= */
	if(*second_pass_flt_rule_hdl != 0)
	{
		IPACMDBG_H("Second pass flt rule was added before, return.\n");
		free(pFilteringTable);
		return IPACM_SUCCESS;
	}

	rt_tbl.ip = IPA_IP_v6;
	snprintf(rt_tbl.name, sizeof(rt_tbl.name), "l2tp");
	IPACMDBG_H("This flt rule points to rt tbl %s.\n", rt_tbl.name);

	if(m_routing.GetRoutingTable(&rt_tbl) == false)
	{
		IPACMERR("Failed to get routing table.\n");
		return IPACM_FAILURE;
	}

	pFilteringTable->ip = IPA_IP_v6;
	pFilteringTable->add_after_hdl = eth_bridge_flt_rule_offset[IPA_IP_v6];

	memset(&flt_rule_entry, 0, sizeof(flt_rule_entry));
	flt_rule_entry.at_rear = 1;

	flt_rule_entry.rule.retain_hdr = 0;
	flt_rule_entry.rule.to_uc = 0;
	flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
	flt_rule_entry.rule.eq_attrib_type = 0;
	flt_rule_entry.rule.rt_tbl_hdl = rt_tbl.hdl;
	flt_rule_entry.rule.hashable = false;	//WLAN->ETH direction rules are set to non-hashable to keep consistent with the other direction

	memcpy(&flt_rule_entry.rule.attrib, &rx_prop->rx[0].attrib, sizeof(flt_rule_entry.rule.attrib));
	flt_rule_entry.rule.attrib.attrib_mask = IPA_FLT_DST_ADDR;

	memcpy(flt_rule_entry.rule.attrib.u.v6.dst_addr, vlan_client_ipv6_addr, sizeof(flt_rule_entry.rule.attrib.u.v6.dst_addr));
	memset(flt_rule_entry.rule.attrib.u.v6.dst_addr_mask, 0xFF, sizeof(flt_rule_entry.rule.attrib.u.v6.dst_addr_mask));

	memcpy(&(pFilteringTable->rules[0]), &flt_rule_entry, sizeof(flt_rule_entry));
	if (false == m_filtering.AddFilteringRuleAfter(pFilteringTable))
	{
		IPACMERR("Failed to add client filtering rules.\n");
		free(pFilteringTable);
		return IPACM_FAILURE;
	}
	*second_pass_flt_rule_hdl = pFilteringTable->rules[0].flt_rule_hdl;

	free(pFilteringTable);
#endif
	return IPACM_SUCCESS;
}

/* delete l2tp flt rule on non l2tp interface */
int IPACM_Lan::del_l2tp_flt_rule(ipa_ip_type iptype, uint32_t first_pass_flt_rule_hdl, uint32_t second_pass_flt_rule_hdl)
{
	if(first_pass_flt_rule_hdl != 0)
	{
		if(m_filtering.DeleteFilteringHdls(&first_pass_flt_rule_hdl, iptype, 1) == false)
		{
			return IPACM_FAILURE;
		}
	}

	if(second_pass_flt_rule_hdl != 0)
	{
		if(m_filtering.DeleteFilteringHdls(&second_pass_flt_rule_hdl, iptype, 1) == false)
		{
			return IPACM_FAILURE;
		}
	}

	return IPACM_SUCCESS;
}

bool IPACM_Lan::is_unique_local_ipv6_addr(uint32_t* ipv6_addr)
{
	uint32_t ipv6_unique_local_prefix, ipv6_unique_local_prefix_mask;

	if(ipv6_addr == NULL)
	{
		IPACMERR("IPv6 address is empty.\n");
		return false;
	}
	IPACMDBG_H("Get ipv6 address with first word 0x%08x.\n", ipv6_addr[0]);

	ipv6_unique_local_prefix = 0xFD000000;
	ipv6_unique_local_prefix_mask = 0xFF000000;
	if((ipv6_addr[0] & ipv6_unique_local_prefix_mask) == (ipv6_unique_local_prefix & ipv6_unique_local_prefix_mask))
	{
		IPACMDBG_H("This IPv6 address is unique local IPv6 address.\n");
		return true;
	}
	return false;
}

/* add tcp syn flt rule */
int IPACM_Lan::add_tcp_syn_flt_rule(ipa_ip_type iptype)
{
	int len;
	struct ipa_flt_rule_add flt_rule_entry;
	ipa_ioc_add_flt_rule *m_pFilteringTable;

	if(rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	len = sizeof(struct ipa_ioc_add_flt_rule) + sizeof(struct ipa_flt_rule_add);
	m_pFilteringTable = (struct ipa_ioc_add_flt_rule *)malloc(len);
	if(!m_pFilteringTable)
	{
		PERROR("Not enough memory.\n");
		return IPACM_FAILURE;
	}
	memset(m_pFilteringTable, 0, len);

	m_pFilteringTable->commit = 1;
	m_pFilteringTable->ep = rx_prop->rx[0].src_pipe;
	m_pFilteringTable->global = false;
	m_pFilteringTable->ip = iptype;
	m_pFilteringTable->num_rules = 1;

	memset(&flt_rule_entry, 0, sizeof(flt_rule_entry));
	flt_rule_entry.at_rear = true;
	flt_rule_entry.rule.retain_hdr = 1;
	flt_rule_entry.flt_rule_hdl = -1;
	flt_rule_entry.status = -1;
	flt_rule_entry.rule.action = IPA_PASS_TO_EXCEPTION;

	memcpy(&flt_rule_entry.rule.attrib, &rx_prop->rx[0].attrib,
		sizeof(flt_rule_entry.rule.attrib));
	flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_TCP_SYN;
	if(iptype == IPA_IP_v4)
	{
		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_PROTOCOL;
		flt_rule_entry.rule.attrib.u.v4.protocol = 6;
	}
	else
	{
		flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_NEXT_HDR;
		flt_rule_entry.rule.attrib.u.v6.next_hdr = 6;
	}

	memcpy(&(m_pFilteringTable->rules[0]), &flt_rule_entry, sizeof(flt_rule_entry));

	if(false == m_filtering.AddFilteringRule(m_pFilteringTable))
	{
		IPACMERR("Error Adding RuleTable(0) to Filtering, aborting...\n");
		free(m_pFilteringTable);
		return IPACM_FAILURE;
	}

	tcp_syn_flt_rule_hdl[iptype] = m_pFilteringTable->rules[0].flt_rule_hdl;
	free(m_pFilteringTable);
	return IPACM_SUCCESS;
}

/* add tcp syn flt rule for l2tp interface*/
int IPACM_Lan::add_tcp_syn_flt_rule_l2tp(ipa_ip_type inner_ip_type)
{
	int len;
	struct ipa_flt_rule_add flt_rule_entry;
	ipa_ioc_add_flt_rule *m_pFilteringTable;

	if(rx_prop == NULL)
	{
		IPACMDBG_H("No rx properties registered for iface %s\n", dev_name);
		return IPACM_SUCCESS;
	}

	len = sizeof(struct ipa_ioc_add_flt_rule) + sizeof(struct ipa_flt_rule_add);
	m_pFilteringTable = (struct ipa_ioc_add_flt_rule *)malloc(len);
	if(!m_pFilteringTable)
	{
		PERROR("Not enough memory.\n");
		return IPACM_FAILURE;
	}
	memset(m_pFilteringTable, 0, len);

	m_pFilteringTable->commit = 1;
	m_pFilteringTable->ep = rx_prop->rx[0].src_pipe;
	m_pFilteringTable->global = false;
	m_pFilteringTable->ip = IPA_IP_v6;
	m_pFilteringTable->num_rules = 1;

	memset(&flt_rule_entry, 0, sizeof(flt_rule_entry));
	flt_rule_entry.at_rear = true;
	flt_rule_entry.rule.retain_hdr = 1;
	flt_rule_entry.flt_rule_hdl = -1;
	flt_rule_entry.status = -1;
	flt_rule_entry.rule.action = IPA_PASS_TO_EXCEPTION;

	memcpy(&flt_rule_entry.rule.attrib, &rx_prop->rx[0].attrib,
		sizeof(flt_rule_entry.rule.attrib));
	flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_TCP_SYN_L2TP;
	if(inner_ip_type == IPA_IP_v4)
	{
		flt_rule_entry.rule.attrib.ether_type = 0x0800;
	}
	else
	{
		flt_rule_entry.rule.attrib.ether_type = 0x86dd;
	}

	memcpy(&(m_pFilteringTable->rules[0]), &flt_rule_entry, sizeof(flt_rule_entry));

	if(false == m_filtering.AddFilteringRule(m_pFilteringTable))
	{
		IPACMERR("Error Adding RuleTable(0) to Filtering, aborting...\n");
		free(m_pFilteringTable);
		return IPACM_FAILURE;
	}

	tcp_syn_flt_rule_hdl[inner_ip_type] = m_pFilteringTable->rules[0].flt_rule_hdl;
	free(m_pFilteringTable);
	return IPACM_SUCCESS;
}
