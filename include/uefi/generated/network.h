/** @file generated/network.h
    Auto-generated from UEFI Specification 2.11.
    Do not edit -- regenerate with scripts/generate-uefi-headers.py
**/

#ifndef AXL_UEFI_GEN_NETWORK_H
#define AXL_UEFI_GEN_NETWORK_H

#include "types.h"
#include "status.h"

typedef union _EFI_IP_ADDRESS {
  UINT32            Addr[4];
  EFI_IPv4_ADDRESS  v4;
  EFI_IPv6_ADDRESS  v6;
}   EFI_IP_ADDRESS;

typedef
EFI_STATUS
(EFIAPI *EFI_SERVICE_BINDING_CREATE_CHILD) (
  IN EFI_SERVICE_BINDING_PROTOCOL                   *This,
  IN OUT EFI_HANDLE                                 *ChildHandle
);

typedef
EFI_STATUS
(EFIAPI *EFI_SERVICE_BINDING_DESTROY_CHILD) (
 IN EFI_SERVICE_BINDING_PROTOCOL             *This,
 IN EFI_HANDLE                               ChildHandle
 );

typedef struct _EFI_SERVICE_BINDING_PROTOCOL {
   EFI_SERVICE_BINDING_CREATE_CHILD            CreateChild;
   EFI_SERVICE_BINDING_DESTROY_CHILD           DestroyChild;
}   EFI_SERVICE_BINDING_PROTOCOL;

typedef struct _EFI_TCP4_ACCESS_POINT {
  BOOLEAN               UseDefaultAddress;
  EFI_IPv4_ADDRESS      StationAddress;
  EFI_IPv4_ADDRESS      SubnetMask;
  UINT16                StationPort;
  EFI_IPv4_ADDRESS      RemoteAddress;
  UINT16                RemotePort;
  BOOLEAN               ActiveFlag;
}   EFI_TCP4_ACCESS_POINT;

typedef struct _EFI_TCP4_OPTION {
  UINT32             ReceiveBufferSize;
  UINT32             SendBufferSize;
  UINT32             MaxSynBackLog;
  UINT32             ConnectionTimeout;
  UINT32             DataRetries;
  UINT32             FinTimeout;
  UINT32             TimeWaitTimeout;
  UINT32             KeepAliveProbes;
  UINT32             KeepAliveTime;
  UINT32             KeepAliveInterval;
  BOOLEAN            EnableNagle;
  BOOLEAN            EnableTimeStamp;
  BOOLEAN            EnableWindowScaling;
  BOOLEAN            EnableSelectiveAck;
  BOOLEAN            EnablePathMtuDiscovery;
}   EFI_TCP4_OPTION;

typedef struct _EFI_TCP4_CONFIG_DATA {
  // Receiving Filters
  // I/O parameters
  UINT8                    TypeOfService;
  UINT8                    TimeToLive;

  // Access Point
  EFI_TCP4_ACCESS_POINT    AccessPoint;

  // TCP Control Options
  EFI_TCP4_OPTION          ControlOption;

}   EFI_TCP4_CONFIG_DATA;

typedef enum {
     Tcp4StateClosed = 0,
     Tcp4StateListen = 1,
     Tcp4StateSynSent = 2,
     Tcp4StateSynReceived = 3,
     Tcp4StateEstablished = 4,
     Tcp4StateFinWait1 = 5,
     Tcp4StateFinWait2 = 6,
     Tcp4StateClosing = 7,
     Tcp4StateTimeWait = 8,
     Tcp4StateCloseWait = 9,
     Tcp4StateLastAck = 10
   }   EFI_TCP4_CONNECTION_STATE;

typedef struct _EFI_TCP4_COMPLETION_TOKEN {
  EFI_EVENT                Event;
  EFI_STATUS               Status;
}   EFI_TCP4_COMPLETION_TOKEN;

typedef struct _EFI_TCP4_CONNECTION_TOKEN {
  EFI_TCP4_COMPLETION_TOKEN      CompletionToken;
}   EFI_TCP4_CONNECTION_TOKEN;

typedef struct _EFI_TCP4_LISTEN_TOKEN {
  EFI_TCP4_COMPLETION_TOKEN      CompletionToken;
  EFI_HANDLE                     NewChildHandle;
}   EFI_TCP4_LISTEN_TOKEN;

typedef struct _EFI_TCP4_FRAGMENT_DATA {
  UINT32                FragmentLength;
  VOID                  *FragmentBuffer;
}   EFI_TCP4_FRAGMENT_DATA;

typedef struct _EFI_TCP4_RECEIVE_DATA {
  BOOLEAN                    UrgentFlag;
  UINT32                     DataLength;
  UINT32                     FragmentCount;
  EFI_TCP4_FRAGMENT_DATA     FragmentTable[1];
}   EFI_TCP4_RECEIVE_DATA;

typedef struct _EFI_TCP4_TRANSMIT_DATA {
  BOOLEAN                     Push;
  BOOLEAN                     Urgent;
  UINT32                      DataLength;
  UINT32                      FragmentCount;
  EFI_TCP4_FRAGMENT_DATA      FragmentTable[1];
}   EFI_TCP4_TRANSMIT_DATA;

typedef struct _EFI_TCP4_IO_TOKEN {
  EFI_TCP4_COMPLETION_TOKEN    CompletionToken;
  union {
    EFI_TCP4_RECEIVE_DATA      *RxData;
    EFI_TCP4_TRANSMIT_DATA     *TxData;
  }                Packet;
} EFI_TCP4_IO_TOKEN;

typedef struct _EFI_TCP4_CLOSE_TOKEN {
  EFI_TCP4_COMPLETION_TOKEN         CompletionToken;
  BOOLEAN                           AbortOnClose;
}   EFI_TCP4_CLOSE_TOKEN;

typedef
EFI_STATUS
(EFIAPI *EFI_TCP4_GET_MODE_DATA) (
  IN EFI_TCP4_PROTOCOL                 *This,
  OUT EFI_TCP4_CONNECTION_STATE        *Tcp4State OPTIONAL,
  OUT EFI_TCP4_CONFIG_DATA             *Tcp4ConfigData OPTIONAL,
  OUT VOID *Ip4ModeData OPTIONAL,
  OUT VOID *MnpConfigData OPTIONAL,
  OUT EFI_SIMPLE_NETWORK_MODE          *SnpModeData OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TCP4_CONFIGURE) (
  IN EFI_TCP4_PROTOCOL           *This,
  IN EFI_TCP4_CONFIG_DATA        *TcpConfigData OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TCP4_ROUTES) (
  IN EFI_TCP4_PROTOCOL  *This,
  IN BOOLEAN            DeleteRoute,
  IN EFI_IPv4_ADDRESS   *SubnetAddress,
  IN EFI_IPv4_ADDRESS   *SubnetMask,
  IN EFI_IPv4_ADDRESS   *GatewayAddress
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TCP4_CONNECT) (
  IN EFI_TCP4_PROTOCOL           *This,
  IN EFI_TCP4_CONNECTION_TOKEN   *ConnectionToken
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TCP4_ACCEPT) (
  IN EFI_TCP4_PROTOCOL        *This,
  IN EFI_TCP4_LISTEN_TOKEN    *ListenToken
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TCP4_TRANSMIT) (
  IN EFI_TCP4_PROTOCOL        *This,
  IN EFI_TCP4_IO_TOKEN        *Token
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TCP4_RECEIVE) (
  IN EFI_TCP4_PROTOCOL        *This,
  IN EFI_TCP4_IO_TOKEN        *Token
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TCP4_CLOSE)(
  IN EFI_TCP4_PROTOCOL             *This,
  IN EFI_TCP4_CLOSE_TOKEN          *CloseToken
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TCP4_CANCEL)(
  IN EFI_TCP4_PROTOCOL              *This,
  IN EFI_TCP4_COMPLETION_TOKEN      *Token OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_TCP4_POLL) (
  IN EFI_TCP4_PROTOCOL        *This
  );

typedef struct _EFI_TCP4_PROTOCOL {
  EFI_TCP4_GET_MODE_DATA         GetModeData;
  EFI_TCP4_CONFIGURE             Configure;
  EFI_TCP4_ROUTES                Routes;
  EFI_TCP4_CONNECT               Connect;
  EFI_TCP4_ACCEPT                Accept;
  EFI_TCP4_TRANSMIT              Transmit;
  EFI_TCP4_RECEIVE               Receive;
  EFI_TCP4_CLOSE                 Close;
  EFI_TCP4_CANCEL                Cancel;
  EFI_TCP4_POLL                  Poll;
}   EFI_TCP4_PROTOCOL;

typedef struct _EFI_IP4_ROUTE_TABLE {
  EFI_IPv4_ADDRESS        SubnetAddress;
  EFI_IPv4_ADDRESS        SubnetMask;
  EFI_IPv4_ADDRESS        GatewayAddress;
}   EFI_IP4_ROUTE_TABLE;

typedef struct _EFI_IP4_CONFIG_DATA {
  UINT8              DefaultProtocol;
  BOOLEAN            AcceptAnyProtocol;
  BOOLEAN            AcceptIcmpErrors;
  BOOLEAN            AcceptBroadcast;
  BOOLEAN            AcceptPromiscuous;
  BOOLEAN            UseDefaultAddress;
  EFI_IPv4_ADDRESS   StationAddress;
  EFI_IPv4_ADDRESS   SubnetMask;
  UINT8              TypeOfService;
  UINT8              TimeToLive;
  BOOLEAN            DoNotFragment;
  BOOLEAN            RawData;
  UINT32             ReceiveTimeout;
  UINT32             TransmitTimeout;
} EFI_IP4_CONFIG_DATA;

typedef struct _EFI_IP4_MODE_DATA {
  BOOLEAN               IsStarted;
  UINT32                MaxPacketSize;
  EFI_IP4_CONFIG_DATA   ConfigData;
  BOOLEAN               IsConfigured;
  UINT32                GroupCount;
  EFI_IPv4_ADDRESS      *GroupTable;
  UINT32                RouteCount;
  EFI_IP4_ROUTE_TABLE   RouteTable;
  UINT32                IcmpTypeCount;
  void  *IcmpTypeList;
}   EFI_IP4_MODE_DATA;

typedef struct _EFI_IP4_HEADER {
  UINT8              HeaderLength:4;
  UINT8              Version:4;
  UINT8              TypeOfService;
  UINT16             TotalLength;
  UINT16             Identification;
  UINT16             Fragmentation;
  UINT8              TimeToLive;
  UINT8              Protocol;
  UINT16             Checksum;
  EFI_IPv4_ADDRESS   SourceAddress;
  EFI_IPv4_ADDRESS   DestinationAddress;
}   EFI_IP4_HEADER;

typedef struct _EFI_IP4_FRAGMENT_DATA {
  UINT32                FragmentLength;
  VOID                  *FragmentBuffer;
}   EFI_IP4_FRAGMENT_DATA;

typedef struct _EFI_IP4_RECEIVE_DATA {
  EFI_TIME              TimeStamp;
  EFI_EVENT             RecycleSignal;
  UINT32                HeaderLength;
  EFI_IP4_HEADER        *Header;
  UINT32                OptionsLength;
  VOID                  *Options;
  UINT32                DataLength;
  UINT32                FragmentCount;
  EFI_IP4_FRAGMENT_DATA FragmentTable[1];
}   EFI_IP4_RECEIVE_DATA;

typedef struct _EFI_IP4_OVERRIDE_DATA {
  EFI_IPv4_ADDRESS         SourceAddress;
  EFI_IPv4_ADDRESS         GatewayAddress;
  UINT8                    Protocol;
  UINT8                    TypeOfService;
  UINT8                    TimeToLive;
  BOOLEAN                  DoNotFragment;
}   EFI_IP4_OVERRIDE_DATA;

typedef struct _EFI_IP4_TRANSMIT_DATA {
  EFI_IPv4_ADDRESS         DestinationAddress;
  EFI_IP4_OVERRIDE_DATA    *OverrideData;
  UINT32                   OptionsLength;
  VOID                     *OptionsBuffer;
  UINT32                   TotalDataLength;
  UINT32                   FragmentCount;
  EFI_IP4_FRAGMENT_DATA    FragmentTable[1];
}   EFI_IP4_TRANSMIT_DATA;

typedef struct _EFI_IP4_COMPLETION_TOKEN {
  EFI_EVENT                  Event;
  EFI_STATUS                 Status;
  union {
    EFI_IP4_RECEIVE_DATA     *RxData;
    EFI_IP4_TRANSMIT_DATA    *TxData;
  }              Packet;
} EFI_IP4_COMPLETION_TOKEN;

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_GET_MODE_DATA) (
  IN EFI_IP4_PROTOCOL                  *This,
  OUT EFI_IP4_MODE_DATA                *Ip4ModeData OPTIONAL,
  OUT VOID *MnpConfigData OPTIONAL,
  OUT EFI_SIMPLE_NETWORK_MODE          *SnpModeData OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_CONFIGURE) (
  IN EFI_IP4_PROTOCOL         *This,
  IN EFI_IP4_CONFIG_DATA      *IpConfigData OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_GROUPS) (
  IN EFI_IP4_PROTOCOL      *This,
  IN BOOLEAN               JoinFlag,
  IN EFI_IPv4_ADDRESS      *GroupAddress OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_ROUTES) (
  IN EFI_IP4_PROTOCOL      *This,
  IN BOOLEAN               DeleteRoute,
  IN EFI_IPv4_ADDRESS      *SubnetAddress,
  IN EFI_IPv4_ADDRESS      *SubnetMask,
  IN EFI_IPv4_ADDRESS      *GatewayAddress
  );

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_TRANSMIT) (
  IN EFI_IP4_PROTOCOL            *This,
  IN EFI_IP4_COMPLETION_TOKEN    *Token
  );

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_RECEIVE) (
  IN EFI_IP4_PROTOCOL            *This,
  IN EFI_IP4_COMPLETION_TOKEN    *Token
  );

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_CANCEL)(
  IN EFI_IP4_PROTOCOL            *This,
  IN EFI_IP4_COMPLETION_TOKEN    *Token OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_POLL) (
  IN EFI_IP4_PROTOCOL         *This
  );

typedef struct _EFI_IP4_PROTOCOL {
  EFI_IP4_GET_MODE_DATA       GetModeData;
  EFI_IP4_CONFIGURE           Configure;
  EFI_IP4_GROUPS              Groups;
  EFI_IP4_ROUTES              Routes;
  EFI_IP4_TRANSMIT            Transmit;
  EFI_IP4_RECEIVE             Receive;
  EFI_IP4_CANCEL              Cancel;
  EFI_IP4_POLL                Poll;
}   EFI_IP4_PROTOCOL;

typedef enum {
  Ip4Config2DataTypeInterfaceInfo,
  Ip4Config2DataTypePolicy,
  Ip4Config2DataTypeManualAddress,
  Ip4Config2DataTypeGateway,
  Ip4Config2DataTypeDnsServer,
  Ip4Config2DataTypeMaximum
} EFI_IP4_CONFIG2_DATA_TYPE;

#define EFI_IP4_CONFIG2_INTERFACE_INFO_NAME_SIZE 32

typedef struct _EFI_IP4_CONFIG2_INTERFACE_INFO {
  CHAR16                           Name[EFI_IP4_CONFIG2_INTERFACE_INFO_NAME_SIZE];
  UINT8                            IfType;
  UINT32                           HwAddressSize;
  EFI_MAC_ADDRESS                  HwAddress;
  EFI_IPv4_ADDRESS                 StationAddress;
  EFI_IPv4_ADDRESS                 SubnetMask;
  UINT32                           RouteTableSize;
  EFI_IP4_ROUTE_TABLE              *RouteTable OPTIONAL;
}   EFI_IP4_CONFIG2_INTERFACE_INFO;

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_CONFIG2_SET_DATA) (
  IN EFI_IP4_CONFIG2_PROTOCOL    *This,
  IN EFI_IP4_CONFIG2_DATA_TYPE   DataType,
  IN UINTN                       DataSize,
  IN VOID                        *Data
  );

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_CONFIG2_GET_DATA) (
  IN EFI_IP4_CONFIG2_PROTOCOL    *This,
  IN EFI_IP4_CONFIG2_DATA_TYPE   DataType,
  IN OUT UINTN                   *DataSize,
  IN VOID                        *Data OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_CONFIG2_REGISTER_NOTIFY) (
  IN EFI_IP4_CONFIG2_PROTOCOL       *This,
  IN EFI_IP4_CONFIG2_DATA_TYPE      DataType,
IN EFI_EVENT                        Event
);

typedef
EFI_STATUS
(EFIAPI *EFI_IP4_CONFIG2_UNREGISTER_NOTIFY) (
  IN EFI_IP4_CONFIG2_PROTOCOL          *This,
  IN EFI_IP4_CONFIG2_DATA_TYPE         DataType,
  IN EFI_EVENT                         Event
  );

typedef struct _EFI_IP4_CONFIG2_PROTOCOL {
  EFI_IP4_CONFIG2_SET_DATA             SetData;
  EFI_IP4_CONFIG2_GET_DATA             GetData;
  EFI_IP4_CONFIG2_REGISTER_NOTIFY      RegisterDataNotify;
  EFI_IP4_CONFIG2_UNREGISTER_NOTIFY    UnregisterDataNotify;
}   EFI_IP4_CONFIG2_PROTOCOL;

typedef struct _EFI_DNS4_CONFIG_DATA {
  UINTN              DnsServerListCount;
  EFI_IPv4_ADDRESS   *DnsServerList;
  BOOLEAN            UseDefaultSetting;
  BOOLEAN            EnableDnsCache;
  UINT8              Protocol;
  EFI_IPv4_ADDRESS   StationIp;
  EFI_IPv4_ADDRESS   SubnetMask;
  UINT16             LocalPort;
  UINT32             RetryCount;
  UINT32             RetryInterval;
}  EFI_DNS4_CONFIG_DATA;

typedef struct _EFI_DNS4_CACHE_ENTRY {
  CHAR16               *HostName;
  EFI_IPv4_ADDRESS     *IpAddress;
   UINT32              Timeout;
}  EFI_DNS4_CACHE_ENTRY;

typedef struct _EFI_DNS4_MODE_DATA {
  EFI_DNS4_CONFIG_DATA       DnsConfigData;
  UINT32                     DnsServerCount;
  EFI_IPv4_ADDRESS           *DnsServerList;
  UINT32                     DnsCacheCount;
  EFI_DNS4_CACHE_ENTRY       *DnsCacheList;
}  EFI_DNS4_MODE_DATA;

typedef struct _DNS_RESOURCE_RECORD {
  CHAR8        *QName;
  UINT16       QType;
  UINT16       QClass;
  UINT32       TTL;
  UINT16       DataLength;
  CHAR8        *RData;
}  DNS_RESOURCE_RECORD;

typedef struct _DNS_HOST_TO_ADDR_DATA {
  UINT32             IpCount;
  EFI_IPv4_ADDRESS   *IpList;
}  DNS_HOST_TO_ADDR_DATA;

typedef struct _DNS_ADDR_TO_HOST_DATA {
 CHAR16                *HostName;
}  DNS_ADDR_TO_HOST_DATA;

typedef struct _DNS_GENERAL_LOOKUP_DATA {
  UINTN                 RRCount;
  DNS_RESOURCE_RECORD  *RRList;
}   DNS_GENERAL_LOOKUP_DATA;

typedef struct _EFI_DNS4_COMPLETION_TOKEN {
  EFI_EVENT                    Event;
  EFI_STATUS                   Status;
  UINT32                       RetryCount;
  UINT32                       RetryInterval;
  union {
    DNS_HOST_TO_ADDR_DATA      *H2AData;
    DNS_ADDR_TO_HOST_DATA      *A2HData;
    DNS_GENERAL_LOOKUP_DATA    *GLookupData;
  }  RspData;
}   EFI_DNS4_COMPLETION_TOKEN;

typedef
EFI_STATUS
(EFIAPI *EFI_DNS4_GET_MODE_DATA)(
  IN EFI_DNS4_PROTOCOL             *This,
  OUT EFI_DNS4_MODE_DATA           *DnsModeData
  );

typedef
EFI_STATUS
(EFIAPI *EFI_DNS4_CONFIGURE)(
  IN EFI_DNS4_PROTOCOL         *This,
  IN EFI_DNS4_CONFIG_DATA      *DnsConfigData
);

typedef
EFI_STATUS
(EFIAPI *EFI_DNS4_HOST_NAME_TO_IP) (
  IN EFI_DNS4_PROTOCOL           *This,
  IN CHAR16                      *HostName,
  IN EFI_DNS4_COMPLETION_TOKEN   *Token
);

typedef
EFI_STATUS
(EFIAPI *EFI_DNS4_IP_TO_HOST_NAME) (
  IN EFI_DNS4_PROTOCOL             *This,
  IN EFI_IPv4_ADDRESS              IpAddress,
  IN EFI_DNS4_COMPLETION_TOKEN     *Token
);

typedef
EFI_STATUS
(EFIAPI *EFI_DNS4_GENERAL_LOOKUP) (
  IN EFI_DNS4_PROTOCOL             *This,
  IN CHAR8                         *QName,
  IN UINT16                        QType,
  IN UINT16                        QClass,
  IN EFI_DNS4_COMPLETION_TOKEN     *Token
);

typedef
 EFI_STATUS
 (EFIAPI *EFI_DNS4_UPDATE_DNS_CACHE) (
   IN EFI_DNS4_PROTOCOL             *This,
   IN BOOLEAN                       DeleteFlag,
   IN BOOLEAN                       Override,
   IN EFI_DNS4_CACHE_ENTRY          DnsCacheEntry
);

typedef
EFI_STATUS
(EFIAPI *EFI_DNS4_POLL) (
 IN EFI_DNS4_PROTOCOL        *This
);

typedef
EFI_STATUS
(EFIAPI *EFI_DNS4_CANCEL) (
  IN EFI_DNS4_PROTOCOL             *This,
  IN EFI_DNS4_COMPLETION_TOKEN     *Token
);

typedef struct _EFI_DNS4_PROTOCOL {
  EFI_DNS4_GET_MODE_DATA       GetModeData;
  EFI_DNS4_CONFIGURE           Configure;
  EFI_DNS4_HOST_NAME_TO_IP     HostNameToIp;
  EFI_DNS4_IP_TO_HOST_NAME     IpToHostName;
  EFI_DNS4_GENERAL_LOOKUP      GeneralLookUp;
  EFI_DNS4_UPDATE_DNS_CACHE    UpdateDnsCache;
  EFI_DNS4_POLL                Poll;
  EFI_DNS4_CANCEL              Cancel;
}  EFI_DNS4_PROTOCOL;

#define MAX_MCAST_FILTER_CNT                             16

typedef struct _EFI_SIMPLE_NETWORK_MODE {
  UINT32             State;
  UINT32             HwAddressSize;
  UINT32             MediaHeaderSize;
  UINT32             MaxPacketSize;
  UINT32             NvRamSize;
  UINT32             NvRamAccessSize;
  UINT32             ReceiveFilterMask;
  UINT32             ReceiveFilterSetting;
  UINT32             MaxMCastFilterCount;
  UINT32             MCastFilterCount;
  EFI_MAC_ADDRESS    MCastFilter[MAX_MCAST_FILTER_CNT];
  EFI_MAC_ADDRESS    CurrentAddress;
  EFI_MAC_ADDRESS    BroadcastAddress;
  EFI_MAC_ADDRESS    PermanentAddress;
  UINT8              IfType;
  BOOLEAN            MacAddressChangeable;
  BOOLEAN            MultipleTxSupported;
  BOOLEAN            MediaPresentSupported;
  BOOLEAN            MediaPresent;
}   EFI_SIMPLE_NETWORK_MODE;

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_START) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL    *This
    );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_STOP) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL      *This
  );

typedef
  EFI_STATUS
  (EFIAPI *EFI_SIMPLE_NETWORK_INITIALIZE) (
    IN EFI_SIMPLE_NETWORK_PROTOCOL        *This,
    IN UINTN                              ExtraRxBufferSize OPTIONAL,
    IN UINTN                              ExtraTxBufferSize OPTIONAL
    );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_RESET) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL    *This,
  IN BOOLEAN                        ExtendedVerification
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_SHUTDOWN) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL       *This
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_RECEIVE_FILTERS) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL       *This,
  IN UINT32                            Enable,
  IN UINT32                            Disable,
  IN BOOLEAN                           ResetMCastFilter,
  IN UINTN                             MCastFilterCnt OPTIONAL,
  IN EFI_MAC_ADDRESS                   MCastFilter OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_STATION_ADDRESS) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL       *This,
  IN BOOLEAN                           Reset,
  IN EFI_MAC_ADDRESS                   *New OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_STATISTICS) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL       *This,
  IN BOOLEAN                           Reset,
  IN OUT UINTN                         *StatisticsSize OPTIONAL,
  OUT VOID *StatisticsTable OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_MCAST_IP_TO_MAC) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL       *This,
  IN BOOLEAN                           IPv6,
  IN EFI_IP_ADDRESS                    *IP,
  OUT EFI_MAC_ADDRESS                  *MAC
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_NVDATA) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL    *This,
  IN BOOLEAN                        ReadWrite,
  IN UINTN                          Offset,
  IN UINTN                          BufferSize,
  IN OUT VOID                       *Buffer
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_GET_STATUS) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL       *This,
  OUT UINT32                           *InterruptStatus OPTIONAL,
  OUT VOID                             **TxBuf OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_TRANSMIT) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL       *This,
  IN UINTN                             HeaderSize,
  IN UINTN                             BufferSize,
  IN VOID                              *Buffer,
  IN EFI_MAC_ADDRESS                   *SrcAddr OPTIONAL,
  IN EFI_MAC_ADDRESS                   *DestAddr OPTIONAL,
  IN UINT16                            Protocol OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SIMPLE_NETWORK_RECEIVE) (
  IN EFI_SIMPLE_NETWORK_PROTOCOL       *This,
  OUT  UINTN                           *HeaderSize OPTIONAL,
  IN OUT UINTN                         *BufferSize,
  OUT  VOID                            *Buffer,
  OUT  EFI_MAC_ADDRESS                 *SrcAddr OPTIONAL,
  OUT  EFI_MAC_ADDRESS                 *DestAddr OPTIONAL,
  OUT  UINT16                          *Protocol OPTIONAL
  );

typedef struct _EFI_SIMPLE_NETWORK_PROTOCOL {
  UINT64                               Revision;
  EFI_SIMPLE_NETWORK_START             Start;
  EFI_SIMPLE_NETWORK_STOP              Stop;
  EFI_SIMPLE_NETWORK_INITIALIZE        Initialize;
  EFI_SIMPLE_NETWORK_RESET             Reset;
  EFI_SIMPLE_NETWORK_SHUTDOWN          Shutdown;
  EFI_SIMPLE_NETWORK_RECEIVE_FILTERS   ReceiveFilters;
  EFI_SIMPLE_NETWORK_STATION_ADDRESS   StationAddress;
  EFI_SIMPLE_NETWORK_STATISTICS        Statistics;
  EFI_SIMPLE_NETWORK_MCAST_IP_TO_MAC   MCastIpToMac;
  EFI_SIMPLE_NETWORK_NVDATA            NvData;
  EFI_SIMPLE_NETWORK_GET_STATUS        GetStatus;
  EFI_SIMPLE_NETWORK_TRANSMIT          Transmit;
  EFI_SIMPLE_NETWORK_RECEIVE           Receive;
  EFI_EVENT                            WaitForPacket;
  EFI_SIMPLE_NETWORK_MODE              *Mode;
}   EFI_SIMPLE_NETWORK_PROTOCOL;

typedef struct _EFI_DHCP4_PACKET_OPTION {
  UINT8      OpCode;
  UINT8      Length;
  UINT8      Data[1];
}  EFI_DHCP4_PACKET_OPTION;

typedef struct _EFI_DHCP4_HEADER {
  UINT8              OpCode;
  UINT8              HwType;
  UINT8              HwAddrLen;
  UINT8              Hops;
  UINT32             Xid;
  UINT16             Seconds;
  UINT16             Reserved;
  EFI_IPv4_ADDRESS   ClientAddr;
  EFI_IPv4_ADDRESS   YourAddr;
  EFI_IPv4_ADDRESS   ServerAddr;
  EFI_IPv4_ADDRESS   GatewayAddr;
  UINT8              ClientHwAddr[16];
  CHAR8              ServerName[64];
  CHAR8              BootFileName[128];
}   EFI_DHCP4_HEADER;

typedef struct _EFI_DHCP4_PACKET {
  UINT32               Size;
  UINT32               Length;
  struct{
    EFI_DHCP4_HEADER   Header;
    UINT32             Magik;
    UINT8              Option[1];
  }  Dhcp4;
} EFI_DHCP4_PACKET;

typedef enum {
  Dhcp4Stopped     = 0x0,
  Dhcp4Init        = 0x1,
  Dhcp4Selecting   = 0x2,
  Dhcp4Requesting  = 0x3,
  Dhcp4Bound       = 0x4,
  Dhcp4Renewing    = 0x5,
  Dhcp4Rebinding   = 0x6,
  Dhcp4InitReboot  = 0x7,
  Dhcp4Rebooting   = 0x8
}  EFI_DHCP4_STATE;

typedef struct _EFI_DHCP4_CONFIG_DATA {
  UINT32                   DiscoverTryCount;
  UINT32                   *DiscoverTimeout;
  UINT32                   RequestTryCount;
  UINT32                   *RequestTimeout;
  EFI_IPv4_ADDRESS         ClientAddress;
  void  *Dhcp4Callback;
  VOID                     *CallbackContext;
  UINT32                   OptionCount;
  EFI_DHCP4_PACKET_OPTION  **OptionList;
}  EFI_DHCP4_CONFIG_DATA;

typedef struct _EFI_DHCP4_MODE_DATA {
  EFI_DHCP4_STATE            State;
  EFI_DHCP4_CONFIG_DATA      ConfigData;
  EFI_IPv4_ADDRESS           ClientAddress;
  EFI_MAC_ADDRESS            ClientMacAddress;
  EFI_IPv4_ADDRESS           ServerAddress;
  EFI_IPv4_ADDRESS           RouterAddress;
  EFI_IPv4_ADDRESS           SubnetMask;
  UINT32                     LeaseTime;
  EFI_DHCP4_PACKET           *ReplyPacket;
}  EFI_DHCP4_MODE_DATA;

typedef
EFI_STATUS
(EFIAPI *EFI_DHCP4_GET_MODE_DATA)(
  IN EFI_DHCP4_PROTOCOL            *This,
  OUT EFI_DHCP4_MODE_DATA          *Dhcp4ModeData
  );

typedef
EFI_STATUS
(EFIAPI *EFI_DHCP4_CONFIGURE) (
  IN EFI_DHCP4_PROTOCOL            *This,
  IN EFI_DHCP4_CONFIG_DATA         *Dhcp4CfgData OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_DHCP4_START) (
  IN EFI_DHCP4_PROTOCOL        *This,
  IN EFI_EVENT                 CompletionEvent OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_DHCP4_RENEW_REBIND) (
  IN EFI_DHCP4_PROTOCOL          *This,
  IN BOOLEAN                     RebindRequest,
  IN EFI_EVENT                   CompletionEvent OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_DHCP4_RELEASE) (
  IN EFI_DHCP4_PROTOCOL      *This
  );

typedef
EFI_STATUS
(EFIAPI *EFI_DHCP4_STOP) (
  IN EFI_DHCP4_PROTOCOL      *This
  );

typedef
EFI_STATUS
(EFIAPI *EFI_DHCP4_BUILD) (
  IN EFI_DHCP4_PROTOCOL        *This,
  IN EFI_DHCP4_PACKET          *SeedPacket,
  IN UINT32                    DeleteCount,
  IN UINT8                     DeleteList OPTIONAL,
  IN UINT32                    AppendCount,
  IN EFI_DHCP4_PACKET_OPTION   *AppendList[] OPTIONAL,
  OUT EFI_DHCP4_PACKET         **NewPacket
  );

typedef
EFI_STATUS
(EFIAPI *EFI_DHCP4_TRANSMIT_RECEIVE) (
  IN EFI_DHCP4_PROTOCOL                  *This,
  IN VOID *Token
  );

typedef
EFI_STATUS
(EFIAPI *EFI_DHCP4_PARSE) (
  IN EFI_DHCP4_PROTOCOL            *This,
  IN EFI_DHCP4_PACKET              *Packet,
  IN OUT UINT32                    *OptionCount,
  IN OUT EFI_DHCP4_PACKET_OPTION   *PacketOptionList[] OPTIONAL
  );

typedef struct _EFI_DHCP4_PROTOCOL {
  EFI_DHCP4_GET_MODE_DATA        GetModeData;
  EFI_DHCP4_CONFIGURE            Configure;
  EFI_DHCP4_START                Start;
  EFI_DHCP4_RENEW_REBIND         RenewRebind;
  EFI_DHCP4_RELEASE              Release;
  EFI_DHCP4_STOP                 Stop;
  EFI_DHCP4_BUILD                Build;
  EFI_DHCP4_TRANSMIT_RECEIVE     TransmitReceive;
  EFI_DHCP4_PARSE                Parse;
}  EFI_DHCP4_PROTOCOL;

typedef struct _EFI_UDP4_SESSION_DATA {
  EFI_IPv4_ADDRESS         SourceAddress;
  UINT16                   SourcePort;
  EFI_IPv4_ADDRESS         DestinationAddress;
  UINT16                   DestinationPort;
}  EFI_UDP4_SESSION_DATA;

typedef struct _EFI_UDP4_FRAGMENT_DATA {
  UINT32             FragmentLength;
  VOID               *FragmentBuffer;
}  EFI_UDP4_FRAGMENT_DATA;

typedef struct _EFI_UDP4_RECEIVE_DATA {
  EFI_TIME                  TimeStamp;
  EFI_EVENT                 RecycleSignal;
  EFI_UDP4_SESSION_DATA     UdpSession;
  UINT32                    DataLength;
  UINT32                    FragmentCount;
  EFI_UDP4_FRAGMENT_DATA    FragmentTable[1];
}   EFI_UDP4_RECEIVE_DATA;

typedef struct _EFI_UDP4_TRANSMIT_DATA {
  EFI_UDP4_SESSION_DATA      *UdpSessionData;
  EFI_IPv4_ADDRESS           *GatewayAddress;
  UINT32                     DataLength;
  UINT32                     FragmentCount;
  EFI_UDP4_FRAGMENT_DATA     FragmentTable[1];
}  EFI_UDP4_TRANSMIT_DATA;

typedef struct _EFI_UDP4_CONFIG_DATA {
  //Receiving Filters
  BOOLEAN              AcceptBroadcast;
  BOOLEAN              AcceptPromiscuous;
  BOOLEAN              AcceptAnyPort;
  BOOLEAN              AllowDuplicatePort;
  // I/O parameters
  UINT8                TypeOfService;
  UINT8                TimeToLive;
  BOOLEAN              DoNotFragment;
  UINT32               ReceiveTimeout;
  UINT32               TransmitTimeout;
  // Access Point
  BOOLEAN              UseDefaultAddress;
  EFI_IPv4_ADDRESS     StationAddress;
  EFI_IPv4_ADDRESS     SubnetMask;
  UINT16               StationPort;
  EFI_IPv4_ADDRESS     RemoteAddress;
  UINT16               RemotePort;
}  EFI_UDP4_CONFIG_DATA;

typedef struct _EFI_UDP4_COMPLETION_TOKEN {
  EFI_EVENT                  Event;
  EFI_STATUS                 Status;
  union {
    EFI_UDP4_RECEIVE_DATA    *RxData;
    EFI_UDP4_TRANSMIT_DATA   *TxData;
  }            Packet;
}  EFI_UDP4_COMPLETION_TOKEN;

typedef
EFI_STATUS
(EFIAPI *EFI_UDP4_GET_MODE_DATA) (
  IN EFI_UDP4_PROTOCOL                 *This,
  OUT EFI_UDP4_CONFIG_DATA             *Udp4ConfigData OPTIONAL,
  OUT EFI_IP4_MODE_DATA                *Ip4ModeData OPTIONAL,
  OUT VOID *MnpConfigData OPTIONAL,
  OUT EFI_SIMPLE_NETWORK_MODE          *SnpModeData OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_UDP4_CONFIGURE) (
  IN EFI_UDP4_PROTOCOL         *This,
  IN EFI_UDP4_CONFIG_DATA      *UdpConfigData OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_UDP4_GROUPS) (
  IN EFI_UDP4_PROTOCOL       *This,
  IN BOOLEAN                 JoinFlag,
  IN EFI_IPv4_ADDRESS        *MulticastAddress OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_UDP4_ROUTES) (
  IN EFI_UDP4_PROTOCOL       *This,
  IN BOOLEAN                 DeleteRoute,
  IN EFI_IPv4_ADDRESS        *SubnetAddress,
  IN EFI_IPv4_ADDRESS        *SubnetMask,
  IN EFI_IPv4_ADDRESS        *GatewayAddress
  );

typedef
EFI_STATUS
(EFIAPI *EFI_UDP4_TRANSMIT) (
  IN EFI_UDP4_PROTOCOL               *This,
  IN EFI_UDP4_COMPLETION_TOKEN       *Token
  );

typedef
EFI_STATUS
(EFIAPI *EFI_UDP4_RECEIVE) (
  IN EFI_UDP4_PROTOCOL             *This,
  IN EFI_UDP4_COMPLETION_TOKEN     *Token
  );

typedef
EFI_STATUS
(EFIAPI *EFI_UDP4_CANCEL)(
  IN EFI_UDP4_PROTOCOL           *This,
  IN EFI_UDP4_COMPLETION_TOKEN   *Token OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *EFI_UDP4_POLL) (
  IN EFI_UDP4_PROTOCOL     *This
  );

typedef struct _EFI_UDP4_PROTOCOL {
  EFI_UDP4_GET_MODE_DATA         GetModeData;
  EFI_UDP4_CONFIGURE             Configure;
  EFI_UDP4_GROUPS                Groups;
  EFI_UDP4_ROUTES                Routes;
  EFI_UDP4_TRANSMIT              Transmit;
  EFI_UDP4_RECEIVE               Receive;
  EFI_UDP4_CANCEL                Cancel;
  EFI_UDP4_POLL                  Poll;
}  EFI_UDP4_PROTOCOL;


#endif /* AXL_UEFI_GEN_NETWORK_H */
