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
		IPACM_Wlan.h

		@brief
		This file implements the WLAN iface functionality.

		@Author
		Skylar Chang

*/
#ifndef IPACM_WLAN_H
#define IPACM_WLAN_H

#include <stdio.h>
#include <IPACM_CmdQueue.h>
#include <linux/msm_ipa.h>
#include "IPACM_Routing.h"
#include "IPACM_Filtering.h"
#include "IPACM_Lan.h"
#include "IPACM_Iface.h"
#include "IPACM_Conntrack_NATApp.h"

typedef struct _wlan_client_rt_hdl
{
	uint32_t wifi_rt_rule_hdl_v4;
	uint32_t wifi_rt_rule_hdl_v6[IPV6_NUM_ADDR];
	uint32_t wifi_rt_rule_hdl_v6_wan[IPV6_NUM_ADDR];
}wlan_client_rt_hdl;

typedef struct _ipa_wlan_client
{
	ipacm_event_data_wlan_ex* p_hdr_info;
	uint8_t mac[IPA_MAC_ADDR_SIZE];
	uint32_t v4_addr;
	uint32_t v6_addr[IPV6_NUM_ADDR][4];
	uint32_t hdr_hdl_v4;
	uint32_t hdr_hdl_v6;
	bool route_rule_set_v4;
	int route_rule_set_v6;
	bool ipv4_set;
	int ipv6_set;
	bool ipv4_header_set;
	bool ipv6_header_set;
	bool power_save_set;
#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	bool ipv4_ul_rules_set;
	bool ipv6_ul_rules_set;
	/* store ipv4 UL filter rule handlers from Q6*/
	uint32_t wan_ul_fl_rule_hdl_v4[MAX_WAN_UL_FILTER_RULES];
	/* store ipv6 UL filter rule handlers from Q6*/
	uint32_t wan_ul_fl_rule_hdl_v6[MAX_WAN_UL_FILTER_RULES];
	int8_t lan_stats_idx;
#endif
	wlan_client_rt_hdl wifi_rt_hdl[0]; /* depends on number of tx properties */
}ipa_wlan_client;

/* wlan iface */
class IPACM_Wlan : public IPACM_Lan
{

public:

	IPACM_Wlan(int iface_index);
	virtual ~IPACM_Wlan(void);

	static int total_num_wifi_clients;

	void event_callback(ipa_cm_event_id event, void *data);

	bool is_guest_ap();

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	/* install UL filter rule from Q6 per client */
	int install_uplink_filter_rule_per_client
	(
		ipacm_ext_prop* prop,
		ipa_ip_type iptype,
		uint8_t xlat_mux_id,
		uint8_t *mac_addr
	);

	/* install UL filter rule from Q6 for all clients */
	int install_uplink_filter_rule
	(
		ipacm_ext_prop* prop,
		ipa_ip_type iptype,
		uint8_t xlat_mux_id
	);

	/* Delete UL filter rule from Q6 for all clients */
	int delete_uplink_filter_rule
	(
		ipa_ip_type iptype
	);

	/* Delet UL filter rule from Q6 per client */
	int delete_uplink_filter_rule_per_client
	(
		ipa_ip_type iptype,
		uint8_t *mac_addr
	);

	/* handle lan client connect event. */
	int handle_lan_client_connect(uint8_t *mac_addr);

	/* handle lan client disconnect event. */
	int handle_lan_client_disconnect(uint8_t *mac_addr);

#endif

private:

	bool m_is_guest_ap;

	/* handle wlan access mode switch in ethernet bridging*/
	void eth_bridge_handle_wlan_mode_switch();


	int wlan_client_len;
	ipa_wlan_client *wlan_client;

	int header_name_count;
	int num_wifi_client;

	int wlan_ap_index;

	static int num_wlan_ap_iface;

	NatApp *Nat_App;

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	/* Clients which take HW path. */
	static ipa_lan_client_idx active_lan_client_index[IPA_MAX_NUM_HW_PATH_CLIENTS];
	/* Clients which take SW path. */
	static ipa_lan_client_idx inactive_lan_client_index[IPA_MAX_NUM_HW_PATH_CLIENTS];
#endif

	inline ipa_wlan_client* get_client_memptr(ipa_wlan_client *param, int cnt)
	{
	    char *ret = ((char *)param) + (wlan_client_len * cnt);
		return (ipa_wlan_client *)ret;
	}

	inline int get_wlan_client_index(uint8_t *mac_addr)
	{
		int cnt;
		int num_wifi_client_tmp = num_wifi_client;

		IPACMDBG_H("Passed MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
						 mac_addr[0], mac_addr[1], mac_addr[2],
						 mac_addr[3], mac_addr[4], mac_addr[5]);

		for(cnt = 0; cnt < num_wifi_client_tmp; cnt++)
		{
			IPACMDBG_H("stored MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
							 get_client_memptr(wlan_client, cnt)->mac[0],
							 get_client_memptr(wlan_client, cnt)->mac[1],
							 get_client_memptr(wlan_client, cnt)->mac[2],
							 get_client_memptr(wlan_client, cnt)->mac[3],
							 get_client_memptr(wlan_client, cnt)->mac[4],
							 get_client_memptr(wlan_client, cnt)->mac[5]);

			if(memcmp(get_client_memptr(wlan_client, cnt)->mac,
								mac_addr,
								sizeof(get_client_memptr(wlan_client, cnt)->mac)) == 0)
			{
				IPACMDBG_H("Matched client index: %d\n", cnt);
				return cnt;
			}
		}

		return IPACM_INVALID_INDEX;
	}

	inline int delete_default_qos_rtrules(int clt_indx, ipa_ip_type iptype)
	{
		uint32_t tx_index;
		uint32_t rt_hdl;
		int num_v6;

		if(iptype == IPA_IP_v4)
		{
		     for(tx_index = 0; tx_index < iface_query->num_tx_props; tx_index++)
		     {
		        if((tx_prop->tx[tx_index].ip == IPA_IP_v4) && (get_client_memptr(wlan_client, clt_indx)->route_rule_set_v4==true)) /* for ipv4 */
			{
				IPACMDBG_H("Delete client index %d ipv4 Qos rules for tx:%d \n",clt_indx,tx_index);
				rt_hdl = get_client_memptr(wlan_client, clt_indx)->wifi_rt_hdl[tx_index].wifi_rt_rule_hdl_v4;

				if(m_routing.DeleteRoutingHdl(rt_hdl, IPA_IP_v4) == false)
				{
					return IPACM_FAILURE;
				}
			}
		     } /* end of for loop */

		     /* clean the 4 Qos ipv4 RT rules for client:clt_indx */
		     if(get_client_memptr(wlan_client, clt_indx)->route_rule_set_v4==true) /* for ipv4 */
		     {
				get_client_memptr(wlan_client, clt_indx)->route_rule_set_v4 = false;
		     }
		}

		if(iptype == IPA_IP_v6)
		{
		    for(tx_index = 0; tx_index < iface_query->num_tx_props; tx_index++)
		    {

				if((tx_prop->tx[tx_index].ip == IPA_IP_v6) && (get_client_memptr(wlan_client, clt_indx)->route_rule_set_v6 != 0)) /* for ipv6 */
				{
					for(num_v6 =0;num_v6 < get_client_memptr(wlan_client, clt_indx)->route_rule_set_v6;num_v6++)
					{
						IPACMDBG_H("Delete client index %d ipv6 Qos rules for %d-st ipv6 for tx:%d\n", clt_indx,num_v6,tx_index);
						rt_hdl = get_client_memptr(wlan_client, clt_indx)->wifi_rt_hdl[tx_index].wifi_rt_rule_hdl_v6[num_v6];
						if(m_routing.DeleteRoutingHdl(rt_hdl, IPA_IP_v6) == false)
						{
							return IPACM_FAILURE;
						}

						rt_hdl = get_client_memptr(wlan_client, clt_indx)->wifi_rt_hdl[tx_index].wifi_rt_rule_hdl_v6_wan[num_v6];
						if(m_routing.DeleteRoutingHdl(rt_hdl, IPA_IP_v6) == false)
						{
							return IPACM_FAILURE;
						}
					}

				}
			} /* end of for loop */

		    /* clean the 4 Qos ipv6 RT rules for client:clt_indx */
		    if(get_client_memptr(wlan_client, clt_indx)->route_rule_set_v6 != 0) /* for ipv6 */
		    {
		                 get_client_memptr(wlan_client, clt_indx)->route_rule_set_v6 = 0;
                    }
		}

		return IPACM_SUCCESS;
	}

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
		inline bool is_lan_stats_index_available()
		{
			int cnt;
	
			for(cnt = 0; cnt < IPA_MAX_NUM_HW_PATH_CLIENTS; cnt++)
			{
				if (IPACM_Wlan::active_lan_client_index[cnt].lan_stats_idx == -1) {
					IPACMDBG_H("Available free index :%d\n", cnt);
					return true;
				}
			}
	
			IPACMDBG_H("No free index available\n");
			return false;
		}
	
		inline int8_t get_free_active_lan_stats_index(uint8_t *mac_addr, int ipa_if_num)
		{
			int cnt;
	
			if (!IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
			{
				IPACMDBG_H("LAN stats functionality is not enabled.\n");
				return -1;
			}
	
			IPACMDBG_H("Received mac_addr MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					mac_addr[0], mac_addr[1], mac_addr[2],
					mac_addr[3], mac_addr[4], mac_addr[5]);
	
			for(cnt = 0; cnt < IPA_MAX_NUM_HW_PATH_CLIENTS; cnt++)
			{
				if (IPACM_Wlan::active_lan_client_index[cnt].lan_stats_idx == -1) {
					IPACMDBG_H("Got active lan stats index :%d, reserve it\n", cnt);
					IPACM_Wlan::active_lan_client_index[cnt].lan_stats_idx = cnt;
					memcpy(IPACM_Wlan::active_lan_client_index[cnt].mac,
							mac_addr,
							IPA_MAC_ADDR_SIZE);
					IPACM_Wlan::active_lan_client_index[cnt].ipa_if_num = ipa_if_num;
					return cnt;
				}
			}
	
			IPACMDBG_H("index not available\n");
			return -1;
		}
	
		inline int8_t get_free_inactive_lan_stats_index(uint8_t *mac_addr)
		{
			int cnt;
	
			if (!IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
			{
				IPACMDBG_H("LAN stats functionality is not enabled.\n");
				return -1;
			}
	
			IPACMDBG_H("Received mac_addr MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					mac_addr[0], mac_addr[1], mac_addr[2],
					mac_addr[3], mac_addr[4], mac_addr[5]);
	
			for(cnt = 0; cnt < IPA_MAX_NUM_HW_PATH_CLIENTS; cnt++)
			{
				if (IPACM_Wlan::inactive_lan_client_index[cnt].lan_stats_idx == -1) {
					IPACMDBG_H("Got inactive lan stats index :%d, reserve it\n", cnt);
					IPACM_Wlan::inactive_lan_client_index[cnt].lan_stats_idx = cnt;
					memcpy(IPACM_Wlan::inactive_lan_client_index[cnt].mac,
							mac_addr,
							IPA_MAC_ADDR_SIZE);
					IPACM_Wlan::inactive_lan_client_index[cnt].ipa_if_num = ipa_if_num;
					return cnt;
				}
			}
	
			IPACMDBG_H("index not available\n");
			return -1;
		}
	
		inline int8_t get_lan_stats_index(uint8_t *mac_addr)
		{
			int cnt;
	
			if (!IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
			{
				IPACMDBG_H("LAN stats functionality is not enabled.\n");
				return -1;
			}
	
			IPACMDBG_H("Received mac_addr MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					mac_addr[0], mac_addr[1], mac_addr[2],
					mac_addr[3], mac_addr[4], mac_addr[5]);
	
			for(cnt = 0; cnt < IPA_MAX_NUM_HW_PATH_CLIENTS; cnt++)
			{
				if (memcmp(IPACM_Wlan::active_lan_client_index[cnt].mac,
							mac_addr,
							IPA_MAC_ADDR_SIZE) == 0) {
					IPACMDBG_H("Got lan stats index :%d, return\n", cnt);
					IPACM_Wlan::active_lan_client_index[cnt].lan_stats_idx = cnt;
					memcpy(IPACM_Wlan::active_lan_client_index[cnt].mac,
							mac_addr,
							IPA_MAC_ADDR_SIZE);
					return cnt;
				}
			}
	
			IPACMDBG_H("index not available\n");
			return -1;
		}
	
		inline int get_available_inactive_lan_client(uint8_t *mac_addr, int *ipa_if_num)
		{
			int cnt;
	
			if (!IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
			{
				IPACMDBG_H("LAN stats functionality is not enabled.\n");
				return IPACM_FAILURE;
			}
	
			IPACMDBG_H("Received mac_addr MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					mac_addr[0], mac_addr[1], mac_addr[2],
					mac_addr[3], mac_addr[4], mac_addr[5]);
	
			for(cnt = 0; cnt < IPA_MAX_NUM_HW_PATH_CLIENTS; cnt++)
			{
				if (IPACM_Wlan::inactive_lan_client_index[cnt].lan_stats_idx != -1) {
					IPACMDBG_H("Got inactive lan stats index :%d, return the mac\n", cnt);
					memcpy(mac_addr, IPACM_Wlan::inactive_lan_client_index[cnt].mac, IPA_MAC_ADDR_SIZE);
					*ipa_if_num = IPACM_Wlan::inactive_lan_client_index[cnt].ipa_if_num;
					return IPACM_SUCCESS;
				}
			}
	
			IPACMDBG_H("No inactive client\n");
			return IPACM_FAILURE;
		}
	
		inline int8_t reset_active_lan_stats_index(int8_t idx, uint8_t *mac_addr)
		{
			if (!IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
			{
				IPACMDBG_H("LAN stats functionality is not enabled.\n");
				return IPACM_FAILURE;
			}
	
			IPACMDBG_H("Received mac_addr MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					mac_addr[0], mac_addr[1], mac_addr[2],
					mac_addr[3], mac_addr[4], mac_addr[5]);
	
			if (idx < 0 || idx >= IPA_MAX_NUM_HW_PATH_CLIENTS ||
				memcmp(IPACM_Wlan::active_lan_client_index[idx].mac,
								mac_addr,
								IPA_MAC_ADDR_SIZE))
			{
				IPACMDBG_H("Index :%d invalid\n", idx);
				return IPACM_FAILURE;
			}
			memset(&IPACM_Wlan::active_lan_client_index[idx], -1, sizeof(ipa_lan_client_idx));
			return IPACM_SUCCESS;
		}
	
		inline int8_t reset_inactive_lan_stats_index(uint8_t *mac_addr)
		{
			int cnt;
	
			if (!IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
			{
				IPACMDBG_H("LAN stats functionality is not enabled.\n");
				return IPACM_FAILURE;
			}
	
			IPACMDBG_H("Received mac_addr MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
					mac_addr[0], mac_addr[1], mac_addr[2],
					mac_addr[3], mac_addr[4], mac_addr[5]);
	
			for(cnt = 0; cnt < IPA_MAX_NUM_HW_PATH_CLIENTS; cnt++)
			{
				if (memcmp(IPACM_Wlan::inactive_lan_client_index[cnt].mac,
								mac_addr,
								IPA_MAC_ADDR_SIZE) == 0)
				{
					memset(&IPACM_Wlan::inactive_lan_client_index[cnt], -1, sizeof(ipa_lan_client_idx));
					return IPACM_SUCCESS;
				}
			}
			return IPACM_FAILURE;
		}
	
		inline void reset_lan_stats_index()
		{
			int i;
	
			if (!IPACM_Iface::ipacmcfg->ipacm_lan_stats_enable)
			{
				IPACMDBG_H("LAN stats functionality is not enabled.\n");
				return;
			}
	
			/* Reset everything based on ipa_if_num. */
			for (i = 0; i < IPA_MAX_NUM_HW_PATH_CLIENTS; i++)
			{
				if (IPACM_Wlan::active_lan_client_index[i].ipa_if_num == ipa_if_num)
					memset(&IPACM_Wlan::active_lan_client_index[i], -1, sizeof(ipa_lan_client_idx));
				if (IPACM_Wlan::inactive_lan_client_index[i].ipa_if_num == ipa_if_num)
					memset(&IPACM_Wlan::inactive_lan_client_index[i], -1, sizeof(ipa_lan_client_idx));
			}
		}
	
#endif

	/* for handle wifi client initial,copy all partial headers (tx property) */
	int handle_wlan_client_init_ex(ipacm_event_data_wlan_ex *data);

	/*handle wifi client */
	int handle_wlan_client_ipaddr(ipacm_event_data_all *data);

	/*handle wifi client routing rule*/
	int handle_wlan_client_route_rule(uint8_t *mac_addr, ipa_ip_type iptype);

#ifdef FEATURE_IPACM_PER_CLIENT_STATS
	/*handle wifi client routing rule with rule id*/
	int handle_wlan_client_route_rule_ext(uint8_t *mac_addr, ipa_ip_type iptype);
#endif

	/*handle wifi client power-save mode*/
	int handle_wlan_client_pwrsave(uint8_t *mac_addr);

	/*handle wifi client del mode*/
	int handle_wlan_client_down_evt(uint8_t *mac_addr);

	/*handle wlan iface down event*/
	int handle_down_evt();

	/*handle reset wifi-client rt-rules */
	int handle_wlan_client_reset_rt(ipa_ip_type iptype);

	void handle_SCC_MCC_switch(ipa_ip_type);

};


#endif /* IPACM_WLAN_H */
