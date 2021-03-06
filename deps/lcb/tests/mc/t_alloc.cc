#include "mctest.h"

class McAlloc : public ::testing::Test {
protected:
    mc_CMDQUEUE cQueue;

    void setupPipeline(mc_PIPELINE *pipeline) {
        mcreq_queue_init(&cQueue);
        mcreq_pipeline_init(pipeline);
        pipeline->parent = &cQueue;
    }
};

TEST_F(McAlloc, testPipelineFreeAlloc)
{
    mc_PIPELINE pipeline;
    memset(&pipeline, 0, sizeof(pipeline));
    mcreq_pipeline_init(&pipeline);
    mcreq_pipeline_cleanup(&pipeline);
}

TEST_F(McAlloc, testPacketFreeAlloc)
{
    mc_PIPELINE pipeline;
    mc_PACKET *copied = NULL;
    memset(&pipeline, 0, sizeof(pipeline));
    setupPipeline(&pipeline);

    mc_PACKET *packet = mcreq_allocate_packet(&pipeline);
    ASSERT_TRUE(packet != NULL);

    mcreq_reserve_header(&pipeline, packet, 24);

    // Check to see that we can also detach a packet and use it after the
    // other resources have been released
    copied = mcreq_dup_packet(packet);


    mcreq_wipe_packet(&pipeline, packet);
    mcreq_release_packet(&pipeline, packet);
    mcreq_pipeline_cleanup(&pipeline);

    // Write to the detached packet. Ensure we don't crash
    memset(SPAN_BUFFER(&copied->kh_span), 0xff, copied->kh_span.size);
    mcreq_wipe_packet(NULL, copied);
    mcreq_release_packet(NULL, copied);
}


TEST_F(McAlloc, testKeyAlloc)
{
    CQWrap q;
    mc_PACKET *packet;
    mc_PIPELINE *pipeline;
    lcb_CMDBASE cmd;

    protocol_binary_request_header hdr;
    memset(&cmd, 0, sizeof(cmd));
    memset(&hdr, 0, sizeof(hdr));

    cmd.key.contig.bytes = const_cast<char *>("Hello");
    cmd.key.contig.nbytes = 5;

    lcb_error_t ret;
    ret = mcreq_basic_packet(&q, &cmd, &hdr, 0, &packet, &pipeline);
    ASSERT_EQ(LCB_SUCCESS, ret);
    ASSERT_TRUE(packet != NULL);
    ASSERT_TRUE(pipeline != NULL);
    ASSERT_EQ(5, ntohs(hdr.request.keylen));

    int vb = vbucket_get_vbucket_by_key(q.config, "Hello", 5);
    ASSERT_EQ(vb, ntohs(hdr.request.vbucket));

    // Copy the header
    memcpy(SPAN_BUFFER(&packet->kh_span), &hdr, sizeof(hdr));

    lcb_VALBUF vreq;
    memset(&vreq, 0, sizeof(vreq));

    const void *key;
    lcb_size_t nkey;
    // Get back the key we just placed inside the header
    mcreq_get_key(packet, &key, &nkey);
    ASSERT_EQ(5, nkey);
    ASSERT_EQ(0, memcmp(key, "Hello", 5));

    mcreq_wipe_packet(pipeline, packet);
    mcreq_release_packet(pipeline, packet);
}

// Check that our value allocation stuff works. This only tests copied values
TEST_F(McAlloc, testValueAlloc)
{
    CQWrap q;
    mc_PACKET *packet;
    mc_PIPELINE *pipeline;
    lcb_CMDBASE cmd;
    protocol_binary_request_header hdr;
    lcb_VALBUF vreq;

    memset(&cmd, 0, sizeof(cmd));
    memset(&hdr, 0, sizeof(hdr));
    memset(&vreq, 0, sizeof(vreq));

    const char *key = "Hello";
    const char *value = "World";


    lcb_error_t ret;
    cmd.key.contig.bytes = const_cast<char *>(key);
    cmd.key.contig.nbytes = 5;
    vreq.u_buf.contig.bytes = const_cast<char *>(value);
    vreq.u_buf.contig.nbytes = 5;

    ret = mcreq_basic_packet(&q, &cmd, &hdr, 0, &packet, &pipeline);
    ASSERT_EQ(LCB_SUCCESS, ret);
    ret = mcreq_reserve_value(pipeline, packet, &vreq);
    ASSERT_EQ(ret, LCB_SUCCESS);
    ASSERT_EQ(packet->flags, MCREQ_F_HASVALUE);

    ASSERT_EQ(0, memcmp(SPAN_BUFFER(&packet->u_value.single), value, 5));
    ASSERT_NE(SPAN_BUFFER(&packet->u_value.single), value);
    mcreq_wipe_packet(pipeline, packet);
    mcreq_release_packet(pipeline, packet);

    // Allocate another packet, but this time, use our own reserved value
    ret = mcreq_basic_packet(&q, &cmd, &hdr, 0, &packet, &pipeline);
    ASSERT_EQ(ret, LCB_SUCCESS);
    vreq.vtype = LCB_KV_CONTIG;
    ret = mcreq_reserve_value(pipeline, packet, &vreq);
    ASSERT_EQ(SPAN_BUFFER(&packet->u_value.single), value);
    ASSERT_EQ(MCREQ_F_HASVALUE|MCREQ_F_VALUE_NOCOPY, packet->flags);
    mcreq_wipe_packet(pipeline, packet);
    mcreq_release_packet(pipeline, packet);


    nb_IOV iov[2];
    iov[0].iov_base = (void *)value;
    iov[0].iov_len = 3;
    iov[1].iov_base = (void *)(value + 3);
    iov[1].iov_len = 2;

    vreq.u_buf.multi.iov = (lcb_IOV *)iov;
    vreq.u_buf.multi.niov = 2;
    vreq.vtype = LCB_KV_IOV;
    ret = mcreq_basic_packet(&q, &cmd, &hdr, 0, &packet, &pipeline);
    ASSERT_EQ(LCB_SUCCESS, ret);
    ret = mcreq_reserve_value(pipeline, packet, &vreq);
    ASSERT_EQ(LCB_SUCCESS, ret);
    ASSERT_EQ(MCREQ_F_HASVALUE|MCREQ_F_VALUE_IOV|MCREQ_F_VALUE_NOCOPY,
              packet->flags);
    ASSERT_NE(&iov[0], (nb_IOV *)packet->u_value.multi.iov);
    ASSERT_EQ(2, packet->u_value.multi.niov);
    ASSERT_EQ(5, packet->u_value.multi.total_length);
    mcreq_wipe_packet(pipeline, packet);
    mcreq_release_packet(pipeline, packet);
}
