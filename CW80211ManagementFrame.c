/**************************************
 * 
 *  Elena Agostini elena.ago@gmail.com
 * 	802.11 Management Frame
 * 
 ***************************************/
#include "CWWTP.h"




/* ------------------------------------------------ */


CW_THREAD_RETURN_TYPE CWWTPBSSManagement(void *arg){
	struct WTPBSSInfo * BSSInfo = (struct WTPBSSInfo *) arg;
	
	CWLog("Dentro Thread ssid: %s", BSSInfo->interfaceInfo->SSID);

	//Start reading from AP readers
	CW80211ManagementFrameEvent(&(BSSInfo->interfaceInfo->nl_mgmt), CW80211EventReceive, BSSInfo->interfaceInfo->nl_cb);
}

void CW80211ManagementFrameEvent(struct nl_handle **handle, cw_sock_handler handler, void * cb)
{
	//Set file descriptor of socket to non-blocking state
	nl_socket_set_nonblocking(*handle);
	int nlSocketFD = nl_socket_get_fd(*handle);
	while(1)
	{
		int result;
		fd_set readset;
		do {
		   FD_ZERO(&readset);
		   FD_SET(nlSocketFD, &readset);
		   result = select(nlSocketFD + 1, &readset, NULL, NULL, NULL);
		} while (result == -1 && errno == EINTR);
		
		if (result > 0) {
		   if (FD_ISSET(nlSocketFD, &readset)) {
			   
				//The nlSocketFD has data available to be read
			 handler(cb, (*handle));
		   }
		}
		else if (result < 0) {
		   CWLog("Error on select(): %s", strerror(errno));
		}
	}			     
}

void CW80211EventReceive(void *cbPtr, void *handlePtr)
{
	struct nl_cb *cb = (struct nl_cb *) cbPtr;
	struct nl_handle * handle = (struct nl_handle *) handlePtr;
	
	int res;

	CWLog("nl80211: Event message available");
	res = nl_recvmsgs(handle, cb);
	if (res < 0) {
		CWLog("nl80211: %s->nl_recvmsgs failed: %d, %s",  __func__, res, strerror(res));
	}
}

void CW80211EventProcess(WTPBSSInfo * WTPBSSInfoPtr, int cmd, struct nlattr **tb)
{
	char * frameResponse = NULL;
	WTPSTAInfo * thisSTA;
	
	u64 cookie_out;
	int frameRespLen=0, offsetFrameReceived=0;
	short int fc, stateSTA = CW_80211_STA_OFF;
	int frameLen;
	CWLog("nl80211: Drv Event %d (%s) received for %s", cmd, nl80211_command_to_string(cmd), WTPBSSInfoPtr->interfaceInfo->ifName);
	
	//union wpa_event_data data;
	if(tb[NL80211_ATTR_FRAME])
		frameLen = nla_len(tb[NL80211_ATTR_FRAME]);
	else
	{
		CWLog("[NL80211] Unexpected frame");
		return;
	}
	unsigned char frameReceived[frameLen+1];
	
	CW_COPY_MEMORY(frameReceived, nla_data(tb[NL80211_ATTR_FRAME]), frameLen);
	
	if(!CW80211ParseFrameIEControl(frameReceived, &(offsetFrameReceived), &fc))
		return;
	
	/* +++ PROBE Request/Response +++ */
	if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_MGMT && WLAN_FC_GET_STYPE(fc) == WLAN_FC_STYPE_PROBE_REQ)
	{
		CWLog("[80211] Probe Request Received");
		struct CWFrameProbeRequest probeRequest;
		if(!CW80211ParseProbeRequest(frameReceived, &probeRequest))
		{
			CWErrorRaise(CW_ERROR_OUT_OF_MEMORY, NULL);
			return;
		}
		
		if(strcmp(probeRequest.SSID, WTPBSSInfoPtr->interfaceInfo->SSID))
		{
			CWLog("[80211] SSID is not the same of this interface. Aborted");
			return;
		}
		
		thisSTA = addSTABySA(WTPBSSInfoPtr, probeRequest.SA);
		if(thisSTA)
			thisSTA->state = CW_80211_STA_PROBE;
		else
			CWLog("[CW80211] Problem adding STA %02x:%02x:%02x:%02x:%02x:%02x", (int) probeRequest.SA[0], (int) probeRequest.SA[1], (int) probeRequest.SA[2], (int) probeRequest.SA[3], (int) probeRequest.SA[4], (int) probeRequest.SA[5]);
		
		//Split MAC: invia probe request ad AC per conoscenza
#ifdef SPLIT_MAC
		if(!CWSendFrameMgmtFromWTPtoAC(frameReceived, frameLen))
			return;
#endif

//In ogni caso, risponde il WTP direttamente senza attendere AC
		frameResponse = CW80211AssembleProbeResponse(WTPBSSInfoPtr, &(probeRequest), &frameRespLen);
	}
	
	/* +++ AUTH +++ */
	if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_MGMT && WLAN_FC_GET_STYPE(fc) == WLAN_FC_STYPE_AUTH)
	{
		CWLog("[80211] Auth Request Received");
		
		struct CWFrameAuthRequest authRequest;
		if(!CW80211ParseAuthRequest(frameReceived, &authRequest))
		{
			CWErrorRaise(CW_ERROR_OUT_OF_MEMORY, NULL);
			return;
		}
		
		thisSTA = findSTABySA(WTPBSSInfoPtr, authRequest.SA);
		if(thisSTA)
		{
			if(thisSTA->state == CW_80211_STA_PROBE)
				thisSTA->state = CW_80211_STA_AUTH;
			else
			{
				CWLog("[CW80211] STA %02x:%02x:%02x:%02x:%02x:%02x hasn't send a Probe Request before sending Auth Request.", (int) authRequest.SA[0], (int) authRequest.SA[1], (int) authRequest.SA[2], (int) authRequest.SA[3], (int) authRequest.SA[4], (int) authRequest.SA[5]);
				return;
			}
		}
		else
		{
			CWLog("[CW80211] Problem adding STA %02x:%02x:%02x:%02x:%02x:%02x", (int) authRequest.SA[0], (int) authRequest.SA[1], (int) authRequest.SA[2], (int) authRequest.SA[3], (int) authRequest.SA[4], (int) authRequest.SA[5]);
			return CW_FALSE;
		}
		
		//Split MAC: invia auth ad AC ed attende il frame di risposta
#ifdef SPLIT_MAC
		if(!CWSendFrameMgmtFromWTPtoAC(frameReceived, frameLen))
			return;
#endif
		//Local MAC: invia direttamente auth a STA
#ifndef SPLIT_MAC
		frameResponse = CW80211AssembleAuthResponse(WTPBSSInfoPtr->interfaceInfo->MACaddr, &authRequest, &frameRespLen);
#endif
	}
	
	/* +++ Association Response +++ */
	if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_MGMT && WLAN_FC_GET_STYPE(fc) == WLAN_FC_STYPE_ASSOC_REQ)
	{
		CWLog("[80211] Association Request Received");
		struct CWFrameAssociationRequest assocRequest;
		if(!CW80211ParseAssociationRequest(frameReceived, &assocRequest))
		{
			CWErrorRaise(CW_ERROR_OUT_OF_MEMORY, NULL);
			return;
		}
		
		thisSTA = findSTABySA(WTPBSSInfoPtr, assocRequest.SA);
		if(thisSTA)
		{
			if(thisSTA->state == CW_80211_STA_AUTH)
				thisSTA->state = CW_80211_STA_ASSOCIATION;
			else
			{
				CWLog("[CW80211] STA %02x:%02x:%02x:%02x:%02x:%02x hasn't send an Auth Request before sending Association Request.", (int) assocRequest.SA[0], (int) assocRequest.SA[1], (int) assocRequest.SA[2], (int) assocRequest.SA[3], (int) assocRequest.SA[4], (int) assocRequest.SA[5]);
				return;
			}
		}
		else
		{
			CWLog("[CW80211] Problem adding STA %02x:%02x:%02x:%02x:%02x:%02x", (int) assocRequest.SA[0], (int) assocRequest.SA[1], (int) assocRequest.SA[2], (int) assocRequest.SA[3], (int) assocRequest.SA[4], (int) assocRequest.SA[5]);
			return CW_FALSE;
		}
		thisSTA->capabilityBit = assocRequest.capabilityBit;
		thisSTA->listenInterval = assocRequest.listenInterval;
		thisSTA->lenSupportedRates = assocRequest.supportedRatesLen;
		
		CW_CREATE_ARRAY_CALLOC_ERR(thisSTA->supportedRates, thisSTA->lenSupportedRates+1, char, {CWErrorRaise(CW_ERROR_OUT_OF_MEMORY, NULL); return CW_FALSE;});
		CW_COPY_MEMORY(thisSTA->supportedRates, assocRequest.supportedRates, thisSTA->lenSupportedRates);

		//Send Association Frame
		if(!CWSendFrameMgmtFromWTPtoAC(frameReceived, frameLen))
			return;
		
		//Local MAC
#ifndef SPLIT_MAC
		//Ass ID is a random number
		CW80211SetAssociationID(&(thisSTA->staAID));
		frameResponse = CW80211AssembleAssociationResponse(WTPBSSInfoPtr, thisSTA, &assocRequest, &frameRespLen);
		//Send Association Frame Response
		if(!CWSendFrameMgmtFromWTPtoAC(frameResponse, frameRespLen))
			return;
#endif
	}
	
	if(frameResponse)
	{
		if(!CW80211SendFrame(WTPBSSInfoPtr, 0, CW_FALSE, frameResponse, frameRespLen, &(cookie_out), 1,1))
			CWLog("NL80211: Errore CW80211SendFrame");
	}
	
	/* +++ Dissassociation or Deauthentication Frame: cleanup of STA parameters +++ */
	if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_MGMT && (WLAN_FC_GET_STYPE(fc) == WLAN_FC_STYPE_DEAUTH || WLAN_FC_GET_STYPE(fc) == WLAN_FC_STYPE_DISASSOC))
	{
		CWLog("[CW80211] Deauth/Disassociation Request Received");
		struct CWFrameDeauthDisassociationRequest disassocRequest;
		if(!CW80211ParseDeauthDisassociationRequest(frameReceived, &disassocRequest))
		{
			CWErrorRaise(CW_ERROR_OUT_OF_MEMORY, NULL);
			return;
		}
		
		if(!delSTABySA(WTPBSSInfoPtr, disassocRequest.SA))
			CWLog("[CW80211] Problem deleting STA %02x:%02x:%02x:%02x:%02x:%02x", (int) disassocRequest.SA[0], (int) disassocRequest.SA[1], (int) disassocRequest.SA[2], (int) disassocRequest.SA[3], (int) disassocRequest.SA[4], (int) disassocRequest.SA[5]);
	}
}

WTPSTAInfo * addSTABySA(WTPBSSInfo * WTPBSSInfoPtr, char * sa) {
	int indexSTA;
	
	if(sa == NULL)
		return NULL;
		
	for(indexSTA=0; indexSTA < WTP_MAX_STA; indexSTA++)
	{
		//Se gia c'era, riazzero tutto
		if(WTPBSSInfoPtr->staList[indexSTA].address != NULL && !strcmp(WTPBSSInfoPtr->staList[indexSTA].address, sa))
		{
			WTPBSSInfoPtr->staList[indexSTA].state = CW_80211_STA_OFF;
			return &(WTPBSSInfoPtr->staList[indexSTA]);
		}
	}
	
	for(indexSTA=0; indexSTA < WTP_MAX_STA; indexSTA++)
	{
		if(WTPBSSInfoPtr->staList[indexSTA].address == NULL || WTPBSSInfoPtr->staList[indexSTA].state == CW_80211_STA_OFF)
		{
			CW_CREATE_ARRAY_CALLOC_ERR(WTPBSSInfoPtr->staList[indexSTA].address, ETH_ALEN+1, char, {CWErrorRaise(CW_ERROR_OUT_OF_MEMORY, NULL); return NULL;});
			CW_COPY_MEMORY(WTPBSSInfoPtr->staList[indexSTA].address, sa, ETH_ALEN);
			
			return &(WTPBSSInfoPtr->staList[indexSTA]);
		}
	}
	
	return NULL;
}

WTPSTAInfo * findSTABySA(WTPBSSInfo * WTPBSSInfoPtr, char * sa) {
	int indexSTA;
	
	if(sa == NULL)
		return NULL;
		
	for(indexSTA=0; indexSTA < WTP_MAX_STA; indexSTA++)
	{
		if(WTPBSSInfoPtr->staList[indexSTA].address != NULL && !strcmp(WTPBSSInfoPtr->staList[indexSTA].address, sa))
			return &(WTPBSSInfoPtr->staList[indexSTA]);
	}
	
	return NULL;
}

CWBool delSTABySA(WTPBSSInfo * WTPBSSInfoPtr, char * sa) {
	int indexSTA;
	
	if(sa == NULL)
		return CW_FALSE;
		
	for(indexSTA=0; indexSTA < WTP_MAX_STA; indexSTA++)
	{
		if(WTPBSSInfoPtr->staList[indexSTA].address != NULL && !strcmp(WTPBSSInfoPtr->staList[indexSTA].address, sa))
		{
			CW_FREE_OBJECT(WTPBSSInfoPtr->staList[indexSTA].address);
			WTPBSSInfoPtr->staList[indexSTA].address = NULL;
			WTPBSSInfoPtr->staList[indexSTA].state = CW_80211_STA_OFF;
			//Altro da liberare?
			return CW_TRUE;
		}
	}
	
	return CW_FALSE;
}

CWBool CWSendFrameMgmtFromWTPtoAC(char * frameReceived, int frameLen)
{
	CWProtocolMessage* frameMsg = NULL;
	CWBindingDataListElement* listElement = NULL;
		
	if (!extract802_11_Frame(&frameMsg, frameReceived, frameLen)){
		CWLog("THR FRAME: Error extracting a frameMsg");
		return CW_FALSE;
	}
					
	CWLog("CW80211: Send 802.11 management(len:%d) to AC", frameLen);

	CW_CREATE_OBJECT_ERR(listElement, CWBindingDataListElement, return CW_FALSE;);
	listElement->frame = frameMsg;
	listElement->bindingValues = NULL;
	listElement->frame->data_msgType = CW_IEEE_802_11_FRAME_TYPE; //CW_DATA_MSG_FRAME_TYPE; // CW_IEEE_802_11_FRAME_TYPE;

	CWLockSafeList(gFrameList);
	CWAddElementToSafeListTail(gFrameList, listElement, sizeof(CWBindingDataListElement));
	CWUnlockSafeList(gFrameList);
	
	return CW_TRUE;
}