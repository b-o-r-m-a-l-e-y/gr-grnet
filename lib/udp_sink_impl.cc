/* -*- c++ -*- */
/* 
 * Copyright 2017 ghostop14.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "udp_sink_impl.h"
#include <zlib.h>
#include <boost/array.hpp>

namespace gr {
  namespace grnet {

    udp_sink::sptr
    udp_sink::make(size_t itemsize,size_t vecLen,const std::string &host, int port,int headerType,int payloadsize,bool send_eof)
    {
      return gnuradio::get_initial_sptr
        (new udp_sink_impl(itemsize, vecLen,host, port,headerType,payloadsize,send_eof));
    }

    /*
     * The private constructor
     */
    udp_sink_impl::udp_sink_impl(size_t itemsize,size_t vecLen,const std::string &host, int port,int headerType,int payloadsize,bool send_eof)
      : gr::sync_block("udp_sink",
              gr::io_signature::make(1, 1, itemsize*vecLen),
              gr::io_signature::make(0, 0, 0)),
    d_itemsize(itemsize), d_veclen(vecLen),d_header_type(headerType),d_seq_num(0),d_header_size(0),d_payloadsize(payloadsize),b_send_eof(send_eof)
    {
    	// Lets set up the max payload size for the UDP packet based on the requested payload size.
    	// Some important notes:  For a standard IP/UDP packet, say crossing the Internet with a
    	// standard MTU, 1472 is the max UDP payload size.  Larger values can be sent, however
    	// the IP stack will fragment the packet.  This can cause additional network overhead
    	// as the packet gets reassembled.
    	// Now for local nets that support jumbo frames, the max payload size is 8972 (9000-the UDP 28-byte header)
    	// Same rules apply with fragmentation.

    	d_port = port;

    	d_header_size = 0;

        switch (d_header_type) {
        	case HEADERTYPE_SEQNUM:
        		d_header_size = sizeof(HeaderSeqNum);
        	break;

        	case HEADERTYPE_SEQPLUSSIZE:
        		d_header_size = sizeof(HeaderSeqPlusSize);
        	break;

        	case HEADERTYPE_CHDR:
        		d_header_size = sizeof(CHDR);
        	break;

        	case HEADERTYPE_NONE:
        	break;

        	default:
        		  std::cout << "[UDP Sink] Error: Unknown header type." << std::endl;
        	      exit(1);
        	break;
        }

    	if (d_payloadsize<8) {
  		  std::cout << "[UDP Sink] Error: payload size is too small.  Must be at least 8 bytes once header/trailer adjustments are made." << std::endl;
  	      exit(1);
    	}

    	d_block_size = d_itemsize * d_veclen;

    	localBuffer = new unsigned char[d_payloadsize];

        std::string s__port = (boost::format("%d") % port).str();
        std::string s__host = host.empty() ? std::string("localhost") : host;
        boost::asio::ip::udp::resolver resolver(d_io_service);
        boost::asio::ip::udp::resolver::query query(s__host, s__port,
            boost::asio::ip::resolver_query_base::passive);
        d_endpoint = *resolver.resolve(query);

		udpsocket = new boost::asio::ip::udp::socket(d_io_service);

		// Open will let you transmit UDP without "connecting" to the remote system(s)
		udpsocket->open(boost::asio::ip::udp::v4());

		int outMultiple = (d_payloadsize - d_header_size) / d_block_size;
		gr::block::set_output_multiple(outMultiple);
    }

    /*
     * Our virtual destructor.
     */
    udp_sink_impl::~udp_sink_impl()
    {
    	stop();
    }

    bool udp_sink_impl::stop() {
        if (udpsocket) {
            gr::thread::scoped_lock guard(d_mutex);

        	if (b_send_eof) {
				// Send a few zero-length packets to signal receiver we are done
				boost::array<char, 0> send_buf;
				for(int i = 0; i < 3; i++)
				  udpsocket->send_to(boost::asio::buffer(send_buf), d_endpoint);
        	}

        	udpsocket->close();
        	udpsocket = NULL;

            d_io_service.reset();
            d_io_service.stop();
        }

        if (localBuffer) {
        	delete[] localBuffer;
        	localBuffer = NULL;
        }

        return true;
    }

    void udp_sink_impl::buildHeader() {
        switch (d_header_type) {
        	case HEADERTYPE_SEQNUM:
        	{
        		d_seq_num++;
        		HeaderSeqNum seqHeader;
        		seqHeader.seqnum = d_seq_num;
        		memcpy((void *)tmpHeaderBuff,(void *)&seqHeader,d_header_size);
        	}
        	break;

        	case HEADERTYPE_SEQPLUSSIZE:
        	{
        		d_seq_num++;
        		HeaderSeqPlusSize seqHeaderPlusSize;
        		seqHeaderPlusSize.seqnum = d_seq_num;
        		seqHeaderPlusSize.length = d_payloadsize;
        		memcpy((void *)tmpHeaderBuff,(void *)&seqHeaderPlusSize,d_header_size);
        	}
        	break;

        	case HEADERTYPE_CHDR:
        	{
        		d_seq_num++;

        		// Rollover at 12-bits
        		if (d_seq_num > 0x0FFF)
        			d_seq_num = 1;

        		CHDR chdr;
        		chdr.sid = d_port;
        		chdr.length = d_payloadsize;
        		chdr.seqPlusFlags = d_seq_num;  // For now set all other flags to zero.
        		memcpy((void *)tmpHeaderBuff,(void *)&chdr,d_header_size);
        	}
        	break;
        }
    }

    int
    udp_sink_impl::work_test(int noutput_items,
        gr_vector_const_void_star &input_items,
        gr_vector_void_star &output_items)
    {
        gr::thread::scoped_lock guard(d_mutex);

        long numBytesToTransmit = noutput_items * d_block_size;
        const char *in = (const char *) input_items[0];

        // Build a long local queue to pull from so we can break it up easier
        for (int i=0;i<numBytesToTransmit;i++) {
        	localQueue.push(in[i]);
        }

        // Local boost buffer for transmitting
    	std::vector<boost::asio::const_buffer> transmitbuffer;

    	while (!localQueue.empty()) {
    		// Clear the next transmit buffer
    		transmitbuffer.clear();
    		int bytesAvailable = localQueue.size();
    		int bytesInBuffer = 0;

    		// build our next header if we need it
            if (d_header_type != HEADERTYPE_NONE) {
            	if (d_seq_num == 0xFFFFFFFF)
            		d_seq_num = 0;

            	d_seq_num++;
            	// want to send the header.
            	tmpHeaderBuff[0]=tmpHeaderBuff[1]=tmpHeaderBuff[2]=tmpHeaderBuff[3]=0xFF;


                memcpy((void *)&tmpHeaderBuff[4], (void *)&d_seq_num, sizeof(d_seq_num));

                if ((d_header_type == HEADERTYPE_SEQPLUSSIZE)||(d_header_type == HEADERTYPE_SEQSIZECRC)) {
                	// NOTE: Header size field includes the whole data block including the header.
                	if (bytesAvailable >= d_payloadsize)
                        memcpy((void *)&tmpHeaderBuff[8], (void *)&d_payloadsize, sizeof(d_payloadsize));
                    else {
                    	int newSize = bytesAvailable + d_header_size;
                        memcpy((void *)&tmpHeaderBuff[8], (void *)&newSize, sizeof(newSize));
                    }
                }

                transmitbuffer.push_back(boost::asio::buffer((const void *)tmpHeaderBuff, d_header_size));
                bytesInBuffer = d_header_size;
            }

            int bytesRemaining = d_payloadsize - bytesInBuffer;

            if (bytesRemaining > bytesAvailable)
            	bytesRemaining = bytesAvailable;

            for (int i=0;i<bytesRemaining;i++) {
            	localBuffer[i] = localQueue.front();
            	localQueue.pop();
            }

        	transmitbuffer.push_back(boost::asio::buffer((const void *)localBuffer, bytesRemaining));
            udpsocket->send_to(transmitbuffer,d_endpoint);
    	}

        return noutput_items;
    }

    int
    udp_sink_impl::work(int noutput_items,
        gr_vector_const_void_star &input_items,
        gr_vector_void_star &output_items)
    {
        gr::thread::scoped_lock guard(d_mutex);

        long numBytesToTransmit = noutput_items * d_block_size;
        const char *in = (const char *) input_items[0];

        // Build a long local queue to pull from so we can break it up easier
        for (int i=0;i<numBytesToTransmit;i++) {
        	localQueue.push(in[i]);
        }

        // Local boost buffer for transmitting
    	std::vector<boost::asio::const_buffer> transmitbuffer;

    	while (!localQueue.empty()) {
    		// Clear the next transmit buffer
    		transmitbuffer.clear();
    		int bytesAvailable = localQueue.size();
    		int bytesInBuffer = 0;

    		// build our next header if we need it
            if (d_header_type != HEADERTYPE_NONE) {
            	buildHeader();
                transmitbuffer.push_back(boost::asio::buffer((const void *)tmpHeaderBuff, d_header_size));
                bytesInBuffer = d_header_size;
            }

            int bytesRemaining = d_payloadsize - bytesInBuffer;

            if (bytesRemaining > bytesAvailable)
            	bytesRemaining = bytesAvailable;

            for (int i=0;i<bytesRemaining;i++) {
            	localBuffer[i] = localQueue.front();
            	localQueue.pop();
            }

        	transmitbuffer.push_back(boost::asio::buffer((const void *)localBuffer, bytesRemaining));
            udpsocket->send_to(transmitbuffer,d_endpoint);
    	}

        return noutput_items;
    }
  } /* namespace grnet */
} /* namespace gr */

