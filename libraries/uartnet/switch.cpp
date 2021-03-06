#include "switch.h"

static LINK links[TOTAL_LINKS];

/******************************/
//Active Monitoring
/******************************/

void check_alive(LINK *link)
{
  int i, j;

  //Loop through every routing table entry for each link. Skipping the 0th entry
  for (j = 1; j < RTABLE_LENGTH; j++)
  {
    //Increment the tick count on every live end node
    if (link->rtable[j].hops == 1)
    {
      //Increment the tick count for the current live node
      ++link->rtable[j].ticks;

      //Mark the node dead if tick threshold has been exceeded
      if (link->rtable[j].ticks >= PING_TICKS_THRESHOLD)
      {
        link->rtable[j].hops = 0;
        printf("ALERT: Node %d is declared dead!\n", j);

        //Broadcast a LEAVE message if switch
      }

      //Ping the node if missed ticks has exceeded to PING_TICKS
      else if (link->rtable[j].ticks >= PING_TICKS)
      {
        printf("Checking if %d is alive\n", j);
        send_hello(0, j, link);
      }
    }
  }
}

void timer1_isr()
{
  int i;
  for (i = 0; i < TOTAL_LINKS; i++)
    check_alive(&links[i]);
}



/******************************/
//Switch Functions
/******************************/

uint8_t create_send_cframe_switch(uint8_t src, uint8_t dst, uint8_t size, uchar *payload)
{
	int i;
	
	//Find which port is the dst reachable at
  for (i = 0; i < TOTAL_LINKS; i++)
  {
    if (links[i].rtable[dst].hops > 0)
      break;
    else if (i == TOTAL_LINKS - 1)
    {
      printf("Could not locate node %d in any routing tables! Dropping...\n", dst);
      return;
    }
  }
  
  //Send out the frame
  create_send_cframe(src, dst, size, payload, &links[i]);
}

uint8_t broadcast(FRAME frame)
{
  uint8_t i, j;
  uint8_t sent_count = 0;
  uchar* pl_orig = frame.payload;

  //Loop through every link in the switch
  for (i = 0; i < TOTAL_LINKS; i++)
  {
    //Do not forward if the other end of the link is uninitialized, or the other end of the link is only the sender
    //if (links[i].end_link_type == UNKNOWN ||(links[i].rtable[frame.src].hops == 1 && links[i].rtable_entries == 1))
    
	//Do not forward if the other end of the link is uninitialized, or the other end of the link can reach the sender
    if (links[i].end_link_type == UNKNOWN || links[i].rtable[frame.src].hops > 0)
      continue;

    //printf("Link %d\n", i);

    //Make a new copy of the payload for each frame, since the transmitter will free them after transmission
    frame.payload = malloc(frame.size);
    memcpy(frame.payload, pl_orig, frame.size);

    //Change the destination to the current endpoint and transmit
    send_frame(frame, &links[i]);
    free(frame.payload);
    ++sent_count;
  }

  //Assuming raw.buf is freed by the caller

  return sent_count;
}


void reset_tick(uint8_t id)
{
  int i;

  if (id == 0 || id == MAX_ADDRESS)
    return;

  for (i = 0; i < TOTAL_LINKS; i++) {
    if (links[i].rtable[id].hops > 0) {
      links[i].rtable[id].ticks = 0;
      break;
    }
  }

  if (i == TOTAL_LINKS && links[i].rtable[id].hops == 0)
    printf("ERROR: Could not find id %d\n", id);
}


//Send rtable with entries from multiple links
uint8_t send_rtbles_msg(uint8_t dst)
{
	uint8_t i, j, writeidx;
	uint8_t entries_added = 0;
	uint8_t total_entries = 0;
	uint8_t pl_size;
	
	//Calculate number of entries
	for(i=0; i<TOTAL_LINKS; i++)
		total_entries += links[i].rtable_entries;
	
	//Calculate the Payload size
	pl_size = LINK_MSG_SIZE + 1 + total_entries * NODE_LENGTH;
	
	//Buffer for preamble + entries
	uchar msg[pl_size];		
	
	//Copy the preamble string to the payload
	strncpy(msg, ROUTING_PREAMBLE, LINK_MSG_SIZE);
	
	//Append the number of routing entries that follows
	msg[LINK_MSG_SIZE] = total_entries;
	
	//Append each of the node information to the payload
	for(i=0; i<TOTAL_LINKS; i++)
	{
		for(j=0; j<RTABLE_LENGTH; j++)
		{
			if(links[i].rtable[j].hops > 0)
			{
				//Increment the write index for the payload
				writeidx = LINK_MSG_SIZE + 1 + NODE_LENGTH*(entries_added++);
				
				//Write the ID
				msg[writeidx] = (uint8_t)j;
				
				//Write and increment the hops
				msg[writeidx + 1] = (uint8_t)(links[i].rtable[j].hops + 1);
			}
		}
	}
	
	
	printf("Written %d entries. Should be %d\n", entries_added, total_entries);
	print_bytes(msg, pl_size);
	printf("\n");
	
	
	//Create and send out an "RTBLE" message
	printf("Sending RTBLE to %d\n", dst);
	create_send_cframe_switch(0, dst, pl_size, msg);
	
	return 0;
}



void proc_raw_frames(RAW_FRAME raw, LINK *link)
{
  uint16_t preamble = *((uint16_t*)&raw.buf[0]);
  uint8_t src = (*((uint8_t*) &raw.buf[2])) & 0x0F;
  uint8_t dest = ((*((uint8_t*) &raw.buf[2])) >> 4);
  uint8_t i, retval;

  FRAME frame;

  //Reset the missed tick count for the sender
  reset_tick(src);

  //Is this packet intended for the switch itself?
  if (dest == 0 || preamble == CFRAME_PREAMBLE)
  {
    //Parse the raw frame
    frame = raw_to_frame(raw);
    free(raw.buf);

    //Handle the control frame
    retval = parse_control_frame(frame, link);

    //Broadcast Join frames
    if (retval == Join_Frame)
    {
      //Reply the sender with a complete routing table
	  send_rtbles_msg(frame.src);
	  
	  printf("Broadcasting JOIN frame to all other nodes\n");

      //Add 1 to hops
      int hops = *((uint8_t*)&frame.payload[LINK_MSG_SIZE]);
      frame.dst = MAX_ADDRESS;
      frame.payload[LINK_MSG_SIZE] = ++hops;
      broadcast(frame);
    }

    free(frame.payload);
    return;
  }

  //Handle broadcast packets
  if (dest == MAX_ADDRESS)
  {
    //Parse the raw frame
    frame = raw_to_frame(raw);
    free(raw.buf);

    printf("Bcast from %u. ", frame.src);
    printf("Forwarded to %u links\n", broadcast(frame));
    free(frame.payload);
    return;
  }



  //Find which port is the dst reachable at
  for (i = 0; i < TOTAL_LINKS; i++)
  {
    if (links[i].rtable[dest].hops > 0)
      break;
    else if (i == TOTAL_LINKS - 1)
    {
      printf("Could not locate node %d in any routing tables! Dropping...\n", dest);
      free(raw.buf);
      return;
    }
  }

  //Forward the frame
  printf("src: %u, dst: %u, olnk: %u\n", src, dest, i);
  add_to_send_queue(raw, &links[i]);

}


uint8_t read_serial_raw(LINK *link)
{
  uint8_t frames_received = 0;
  size_t bytes;
  RAW_FRAME rawframe;

  //See if any new bytes are available for reading
  bytes = check_new_bytes(link);

  //Check if buffer contains one or more complete packets
  if (!(bytes > 0 || link->rbuf_valid))
    return 0;

  //Extract frames from the raw receive buffer
  rawframe = extract_frame_from_rbuf(link);
  while (rawframe.size > 0)
  {
    //Parse the frame and store it in the recvd buffer
    proc_raw_frames(rawframe, link);

    //Check if the rbuf contains more complete packets
    proc_buf(NULL, 0, link);
    rawframe = extract_frame_from_rbuf(link);
    frames_received++;
  }

  return frames_received;
}

/******************************/
//Cframe handlers for switch
/******************************/




/******************************/
//main
/******************************/


void switch_init()
{
  //Setup Timer
  //Timer1.initialize(TICK_MS);
  //Timer1.attachInterrupt(timer1_isr);

  //Initializing link layer data for serial1
  Serial1.begin(115200);
  Serial2.begin(115200);
  Serial3.begin(115200);

  link_init(&Serial1, 0, GATEWAY, &links[0]);
  link_init(&Serial2, 0, GATEWAY, &links[1]);
  link_init(&Serial3, 0, GATEWAY, &links[2]);
  


  //Send out a HELLO message out onto the link
  //send_hello(0, 0, &links[0]);
  //send_hello(0, 0, &links[1]);      //TODO: Sometimes a link connected to an OFF device may lead to loopback. Handle this case.
  //send_hello(0, 0, &links[2]);
}


void switch_task(uint8_t continuous)
{
  int i;

  while (1)
  {
    //Process one serial port at a time
    for (i = 0; i < TOTAL_LINKS; i++)
    {
      //Check if current serial port has any new frames ready for reading
      read_serial_raw(&links[i]);

      //Transmit a packet in the sending queue, if any
      transmit_next(&links[i]);
    }

    delay(100);

    //Only run 1 iteration of send/receive if not in continuous mode
    if (!continuous) break;
  }

}



