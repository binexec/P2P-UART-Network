/*This file must be declared as a .cpp file since it uses the HardwareSerial Object from Arduino's library*/
#include "link.h"

//static uint8_t links_registered = 0;

LINK link_init(HardwareSerial *port, LINK_TYPE link_type)
{
  LINK link;

  link.port = port;
  link.link_type = link_type;
  link.end_link_type = UNKNOWN;
  
  link.rbuf_writeidx = 0;
  link.rbuf_valid = 0;
  link.rbuf_expectedsize = 0;
  link.rqueue_pending = 0;
  link.rqueue_head = 0;
  link.squeue_pending = 0;
  link.squeue_lastsent = 0;
  link.rtable_entries = 0;

  
  memset(link.recvbuf, 0, RECV_BUFFER_SIZE);
  memset(link.recv_queue, 0, RECV_QUEUE_SIZE * sizeof(FRAME));
  memset(link.send_queue, 0, SEND_QUEUE_SIZE * sizeof(RAW_FRAME));
  
  //memset(link.rtable, 0, RTABLE_LENGTH * sizeof(NODE));
  for(int i=0; i<RTABLE_LENGTH; i++ )
  {
	link.rtable[i].id = 0;
	link.rtable[i].hops = 0;
  }
  

  return link;
}






/***************************
RECEIVING BYTES
***************************/


void proc_buf(uchar *rawbuf, size_t chunk_size, LINK *link)
{
  int i;
  uint16_t preamble;
  int valid_bytes;

  //append the new chunk to the appropriate offset of the buffer
  //If function is called without a new chunk, simply check if the current buffer contains pieces of a subsequent packet
  if (rawbuf != NULL && chunk_size > 0)
  {
    memcpy(&link->recvbuf[link->rbuf_writeidx], rawbuf, chunk_size);
    link->rbuf_writeidx += chunk_size;
  }

  //If the buffer is current marked invalid, try and find a preamble to match a new packet
  if (!link->rbuf_valid)
  {
    for (i = 0; i < link->rbuf_writeidx; i++ )
    {
      preamble = *((uint16_t*) &link->recvbuf[i]);
      
      if (preamble == FRAME_PREAMBLE)
      {
        printf("Found a preamble: %X\n", preamble);      
        memcpy(&link->recvbuf[0], &link->recvbuf[i], (RECV_BUFFER_SIZE - i));
        link->rbuf_writeidx -= i;
        link->rbuf_valid = 1;
        break;
      }
    }

    //Flush the buffer if nothing has been found, and the buffer is >50% full
    if (link->rbuf_writeidx >= FLUSH_THRESHOLD && !link->rbuf_valid)
    {
      printf("Flushing %d bytes of unknown raw chunk \n", link->rbuf_writeidx);
      link->rbuf_writeidx = 0;
    }
  }
}




size_t check_complete_frame(LINK *link)
{
  uint16_t preamble = *((uint16_t*)&link->recvbuf[0]);

  if (preamble != FRAME_PREAMBLE)
    return 0;

  //Set the expected payload size if full header has received
  if (link->rbuf_writeidx >= FRAME_HEADER_SIZE)
    link->rbuf_expectedsize = FRAME_HEADER_SIZE + *((uint8_t*)&link->recvbuf[3]) + 2;  //add 2 bytes for "STX" and "ETX" for payload

  //Check total length of raw packet received
  if (link->rbuf_writeidx >= link->rbuf_expectedsize )
    return link->rbuf_expectedsize;
  else
    return 0;

}



size_t check_new_bytes(LINK *link)
{
  int bytes;
  int partial_size, pos;
  uchar tempbuf[RECV_BUFFER_SIZE];

  bytes = link->port->available();
  if (bytes <= 0)
    return 0;

  bytes = link->port->readBytes(tempbuf, bytes);
  
  //Make sure we're not overflowing the buffer
  if (link->rbuf_writeidx + bytes < RECV_BUFFER_SIZE)
  {
    proc_buf(tempbuf, bytes, link);
    pos = 0;
    return bytes;
  }
  else
  {
    //TODO: This part needs more througho testing, and doesn't seem to be working properly
    printf("***too big! cur_idx: %d bytes_pending: %d\n", link->rbuf_writeidx, bytes);
    return 0;
  }
}


RAW_FRAME extract_frame_from_rbuf(LINK *link)
{
  RAW_FRAME raw_frame;
  raw_frame.size = check_complete_frame(link);

  //If the buffer doesn't have any fully received packets, return size 0
  if (raw_frame.size <= 0)
    return raw_frame;

  printf("Complete packet received! %u bytes!\n", raw_frame.size);
  print_bytes(&link->recvbuf[0], raw_frame.size);
  printf("\n");

  //Allocate a new buffer for the raw packet for returning
  raw_frame.buf = malloc(raw_frame.size);
  memcpy(raw_frame.buf, link->recvbuf, raw_frame.size);

  link->rbuf_valid = 0;
  link->rbuf_expectedsize = 0;

  //Move write pointer to the end of the packet
  link->rbuf_writeidx -= raw_frame.size;
  memcpy(&link->recvbuf[0], &link->recvbuf[raw_frame.size], (RECV_BUFFER_SIZE - raw_frame.size));

  return raw_frame;
}




/***************************
STORING RECEIVED FRAMES
***************************/


//Parses a raw frame buffer into a FRAME struct, and store it in the link's received queue. Used by end-nodes only.
uint8_t parse_raw_and_store(RAW_FRAME raw, LINK *link)
{
	uint8_t i, j;
	FRAME frame = raw_to_frame(raw);
	
  //make sure the received queue is not full
  if (link->rqueue_pending == RECV_QUEUE_SIZE)
  {
    printf("Receive Queue is full! Dropping frame...\n");
    free(raw.buf);
    return 0;
  }
 

  //Find an empty slot to store the newly received frame, starting from the last sending index
  for (i = link->rqueue_head, j = 0; j < RECV_QUEUE_SIZE; j++)
  {
    //Wrap index i around to the beginning if needed
    if (i >= RECV_QUEUE_SIZE ) i = 0;
	
	//Check if this spot is marked free
	if (link->recv_queue[i].size == 0) break;
	
	//Increment i to check the next slot
	i++;
  }
  
  //Store the frame
  link->recv_queue[i] = frame;
  link->rqueue_pending++;
  
  /*
  printf("*QUEUED:*\n");
  print_frame(link->recv_queue[i]);
  printf("Buffered into recv_queue pos %d\n", i);
  */
  
  return 1;
	
}

FRAME pop_recv_queue(LINK *link)
{
	FRAME retframe;
	FRAME *curhead;
	
	//Make sure the recv queue has pending data first
	if(link->rqueue_pending == 0)
	{
		retframe.size = 0;
		return retframe;
	}
	
	//Copy the current head out of the recv queue
	curhead = &link->recv_queue[link->rqueue_head];
	memcpy(&retframe, curhead, sizeof(FRAME));
	
	//Advances queue head and cleanup
	if (++link->rqueue_head >= RECV_QUEUE_SIZE) 
		link->rqueue_head = 0;
	
	curhead->size = 0;
	link->rqueue_pending--;
	
	
	//Remember to free the payload


	return retframe;
}




/***************************
SENDING FRAMES
***************************/

uint8_t add_to_send_queue(RAW_FRAME raw, LINK *link)
{
  int i, j;

  if (link->squeue_pending == SEND_QUEUE_SIZE )
  {
    printf("Send Queue is full! Dropping request...\n");
    free(raw.buf);
    return 0;
  }

  //Find a free slot in the sending queue, starting from the last sending index
  for (i = link->squeue_lastsent + 1, j = 0; j < SEND_QUEUE_SIZE; j++)
  {
    //Wrap index i around to the beginning if needed
    if (i >= SEND_QUEUE_SIZE - 1) i = 0;
    else if (i < SEND_QUEUE_SIZE - 1) i++;		

    if (link->send_queue[i].size == 0) break;
  }

  link->send_queue[i] = raw;
  link->squeue_pending++;

  return 1;
}


uint8_t transmit_next(LINK *link)
{
  uint8_t i, j;

  //Check if we have anything to transmit
  if (link->squeue_pending == 0)
    return 0;

  //Find the next packet to transmit, starting from the last sending index
  for (i = link->squeue_lastsent, j = 0; j < SEND_QUEUE_SIZE; j++)
  {
    //Wrap index i around to the beginning if needed
    if (i >= SEND_QUEUE_SIZE - 1) i = 0;
    else if (i < SEND_QUEUE_SIZE - 1) i++;

    if (link->send_queue[i].size > 0) break;
  }

  //printf("PRETENDING to transmit: %d ", i);
  //print_bytes(link->send_queue[i].buf, link->send_queue[i].size);
  
  //Transmit the packet out onto the link
  link->port->write(link->send_queue[i].buf, link->send_queue[i].size);

  
  //cleanup & mark this slot as free
  link->squeue_lastsent = i;
  link->squeue_pending--;
  link->send_queue[i].size = 0;
  free(link->send_queue[i].buf);

  return i;
}

uint8_t send_frame(FRAME frame, LINK *link)
{
	return add_to_send_queue(frame_to_raw(frame), link);
}


uint8_t create_send_frame(uint8_t src, uint8_t dst, uint8_t size, uchar *payload, LINK *link)
{
	return add_to_send_queue(frame_to_raw(create_frame(src, dst, size, payload)), link);
}


/***************************
ROUTING
***************************/


uint8_t update_rtable_entry(uint8_t id, uint8_t hops, LINK *link)
{
	//Make sure the source id is valid
	if(id == 0 || id >= MAX_ADDRESS)
	{
		printf("ERROR: Attempted to add a routing entry for address 0 (link-ctrl) or %d (broadcast)\n", MAX_ADDRESS);
		return 0;
	}
	
	//Make sure routing table isn't full yet (should never happen)
	if(link->rtable_entries >= RTABLE_LENGTH)
	{
		printf("Routing Table Full! Dropping request...\n");
		return 0;
	}

	//Increment the routing table entry count
	if(link->rtable[id].id > 0)
		printf("WARNING: Overwriting existing route entry for node %d\n", id);
	else
		link->rtable_entries++;
	
	//Update the entry
	link->rtable[id].id = id;
	link->rtable[id].hops = hops;
	printf("Updated Routing entry: %d, %d hops\n", link->rtable[id].id, link->rtable[id].hops);
	
	return 1;
}




/***************************
SENDING
***************************/

uint8_t send_probe_msg(uint8_t my_id, LINK *link)
{
	uint8_t pl_size = LINK_MSG_SIZE + 1;		//Buffer for preamble + hops
	uchar msg[pl_size];		
	
	//Copy the preamble string to the payload
	strncpy(msg, PROBE_PREAMBLE, LINK_MSG_SIZE);
	
	//Append my link type after the preamble
	switch(link->link_type)
	{
		case GATEWAY:
			msg[LINK_MSG_SIZE] = 's';
			break;
			
		case ENDPOINT:
			msg[LINK_MSG_SIZE] = 'n';
			break;
		
		//UNKNOWN and other unexpected types
		default:
			msg[LINK_MSG_SIZE] = 0;
	}
	
	print_bytes(msg, LINK_MSG_SIZE + 1);
	
	//Create and send out an "HELLO" message
	create_send_frame(my_id, 0, pl_size, msg, link);
	
	return 0;
}

uint8_t send_join_msg(uint8_t my_id, LINK *link)
{
	uint8_t pl_size = LINK_MSG_SIZE + 1;		//Buffer for preamble + hops
	uchar msg[pl_size];		
	
	//Copy the preamble string to the payload
	strncpy(msg, JOIN_PREAMBLE, LINK_MSG_SIZE);
	
	//Append the initial hop count as 1
	msg[LINK_MSG_SIZE] = 0x01;

	print_bytes(msg, LINK_MSG_SIZE + 1);
	
	//Wait a random period (up to 1 second) before sending
	//delayMicroseconds(rand() % 1000);
	
	//Create and send out an "HELLO" message
	create_send_frame(my_id, 0, pl_size, msg, link);
	
	return 0;
}



uint8_t send_rtble_msg(uint8_t dst, LINK *link)
{
	uint8_t i, j, writeidx;
	
	uint8_t pl_size = LINK_MSG_SIZE + 1 + link->rtable_entries * NODE_LENGTH;
	uchar msg[pl_size];		//Buffer for preamble + entries
	
	//Copy the preamble string to the payload
	strncpy(msg, ROUTING_PREAMBLE, LINK_MSG_SIZE);
	
	//Append the number of routing entries that follows
	msg[LINK_MSG_SIZE] = link->rtable_entries;
	
	//Append each of the node information to the payload
	for(i=0, j=0; i<RTABLE_LENGTH; i++)
	{
		if(link->rtable[i].id > 0)
		{
			//Increment the write index for the payload
			writeidx = LINK_MSG_SIZE + 1 + NODE_LENGTH*(j++);
			
			//Write the ID
			msg[writeidx] = (uint8_t)link->rtable[i].id;
			
			//Write and increment the hops
			msg[writeidx + 1] = (uint8_t)(link->rtable[i].hops + 1);
		}
	}
	
	printf("Written %d entries. Should be %d\n", j, link->rtable_entries);
	print_bytes(msg, pl_size);
	printf("\n");
	
	//Create and send out an "RTBLE" message
	create_send_frame(0, dst, pl_size, msg, link);
	
	return 0;
}


/***************************
PARSING
***************************/

uint8_t parse_probe_msg(FRAME frame, LINK *link)
{
	uint8_t end_id = frame.src;
	uchar end_type = frame.payload[LINK_MSG_SIZE];
	
	//No need to process duplicate HELLO message if the current node has already received a HELLO from this link's other end earlier. This should be changed later to support recovery
	if(link->end_link_type != UNKNOWN)
	{
		printf("Ignoring PROBE from known gateway/endpoint...\n");
		return 0;
	}
	
	printf("Parsed PROBE: %d, type: %c \n", end_id, end_type);
	switch(end_type)
	{
		//Other end is a switch
		case 's':
			link->end_link_type = GATEWAY;
			break;
		
		//Other end is an endpoint. Add the other end's ID into the routing table.
		case 'n':
			link->end_link_type = ENDPOINT;
			update_rtable_entry(end_id, 1, link);
			break;
		
		//Unknown or other unsupported link types
		default:
			link->end_link_type = UNKNOWN;

	}
	
	
	return 1;
}


uint8_t parse_join_msg(FRAME frame, LINK *link)
{
	uint8_t new_id = frame.src;
	uint8_t new_hops = (uint8_t)frame.payload[LINK_MSG_SIZE];
	
	//Add the node's routing information to the table. The same index as its ID is used.
	update_rtable_entry(new_id, new_hops, link);
	printf("Parsed NJOIN: %d, %d hops\n", link->rtable[new_id].id, link->rtable[new_id].hops);
	
	//TODO: If switch, forward the packet to everyone else. Implement in the switch code
	
	//Reply with the current routing table
	send_rtble_msg(new_id, link);
	
	return 1;
}


uint8_t parse_rtble_msg(FRAME frame, LINK *link)
{
	uint8_t entries = (uint8_t)frame.payload[LINK_MSG_SIZE];
	uint8_t i, curid, curhops, readidx;
	
	printf("Parsed RTBLE Message with %d entries!\n", entries);

	
	for(i=0; i<entries; i++)
	{
		//Parse the next routing entry out of the message payload
		readidx = LINK_MSG_SIZE + 1 + NODE_LENGTH*i;
		curid = (uint8_t)frame.payload[readidx];
		curhops = (uint8_t)frame.payload[readidx + 1];
		
		//Insert the current entry from the message into the routing table
		printf("Parsed rtable update entry: %d, %d hops\n", curid, curhops);
		update_rtable_entry(curid, curhops, link);	
	}

	return 0;
}


uint8_t parse_routing_frame(FRAME frame, LINK *link)
{
	if(strncmp(frame.payload, PROBE_PREAMBLE, LINK_MSG_SIZE) == 0)
	{
		printf("Found PROBE message!\n");
		//print_frame(frame);
		return parse_probe_msg(frame, link);
	}
	else if(strncmp(frame.payload, JOIN_PREAMBLE, LINK_MSG_SIZE) == 0)
	{
		printf("Found JOIN message!\n");
		//print_frame(frame);
		return parse_join_msg(frame, link);
	}
	else if(strncmp(frame.payload, ROUTING_PREAMBLE, LINK_MSG_SIZE) == 0)
	{
		printf("Found ROUTE message!\n");
		//print_frame(frame);
		return parse_rtble_msg(frame, link);
	}
	else
	{
		printf("Not a valid routing frame\n");
		print_frame(frame);
		return 0;
	}
		
}



