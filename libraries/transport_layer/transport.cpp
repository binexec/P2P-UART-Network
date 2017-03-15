#include "transport.h"

//Streams that are still in-flight
//static CONNECTION *in_streams;
//static CONNECTION *out_streams;

//Buffer for newly received packets pending to be processed by the application
//static RECEIVED_PACKET *recvd_packets;
//static int recvd_packets_count;


/***************************
  Front-end Functions
***************************/

void network_task()
{
  static uint8_t last_sent = 0;

  while (1)
  {

  }

}

void transport_initialize()
{
  //recvd_packets_count = 0;

  //in_streams = calloc(MAX_INBOUND_STREAMS, sizeof(CONNECTION));
 // out_streams = calloc(MAX_OUTBOUND_STREAMS, sizeof(CONNECTION));
  //recvd_packets = calloc(MAX_RECEIVED_BUF, sizeof(RECEIVED_PACKET));

}

//assumes frame.payload was malloc'd
RECVD_PACKET parse_recvd_frame(FRAME frame, size_t bytes)
{
	RECVD_PACKET recvd;
	uint16_t type = *((uint16_t*)&frame.payload[0]);
	
	switch(type)
	{
		case USPACKET_PREAMBLE:
		
		recvd.type = USPACKET_TYPE;
		recvd.packet.uspacket = frame_to_uspacket(frame);
		break;
		
		case RSPACKET_SYN_PREAMBLE:
		case RSPACKET_ACK_PREAMBLE:
		case RSPACKET_DATA_PREAMBLE:
		
		recvd.type = RSPACKET_TYPE;
		recvd.packet.rspacket = frame_to_rspacket(frame);
		break;
		
		//Treat all else as UMPACKETs
		default:
		recvd.type = UMPACKET_TYPE;
		recvd.packet.umpacket = (UMPACKET) frame;
	}

  return recvd;
}



/***************************
  SENDING SINGLE PACKET
***************************/

int send_umpacket(UMPACKET packet, LINK *link)
{
	//return create_send_frame(packet.src, packet.dst, packet.size, packet.payload, link);
	return send_frame((FRAME)packet, link);
}


int send_uspacket(USPACKET packet, LINK *link)
{
	FRAME frame;
	int retval;
	
	/*** uspacket_to_frame ***/
	
	frame = create_frame(packet.src, packet.dst, (packet.payload_size + USPACKET_HEADER_EXTRA), NULL);
	frame.payload = malloc(frame.size);
	
	//Copy the secondary header fields into the beginning of the payload. Skip src, dst, payload_size
	memcpy(frame.payload, &(((uchar*)&packet)[2]), USPACKET_HEADER_EXTRA);
	
	//Append the remaining payload
	memcpy(&frame.payload[USPACKET_HEADER_EXTRA], packet.payload, packet.payload_size);
	
	/*************************/
	
	
	//Sending the packed uspacket frame
	retval = send_frame(frame, link);
	free(frame.payload);
	
	return retval;
}

int send_rspacket(RSPACKET packet, LINK *link)
{
	FRAME frame;
	int retval;
	size_t actual_pl_size;
	
	/*** rspacket_to_frame ***/

	//Determine the actual payload size for the FRAME. SYN/ACK packets only have the secondary headers in the frame payload.
	if(packet.type == RSPACKET_DATA_PREAMBLE) 
		actual_pl_size = packet.payload_size + RSPACKET_HEADER_EXTRA;
	else 
		actual_pl_size = RSPACKET_HEADER_EXTRA;
	
	//Create a new frame to encapsulate the RSPACKET
	frame = create_frame(packet.src, packet.dst, actual_pl_size, NULL);
	
	//Allocate memory for the frame's payload
	frame.payload = (uchar*)malloc(frame.size);
	
	//Copy the secondary header fields into the beginning of the payload. Skip src, dst, payload_size
	memcpy(frame.payload, &(((uchar*)&packet)[2]), RSPACKET_HEADER_EXTRA);
	
	//Append the remaining payload if it's a DATA packet
	if(packet.type == RSPACKET_DATA_PREAMBLE)
		memcpy(&frame.payload[RSPACKET_HEADER_EXTRA], packet.payload, packet.payload_size);
	
	/*************************/
	
	
	//Sending the packed uspacket frame
	retval = send_frame(frame, link);
	free(frame.payload);
	
	return retval;
}









