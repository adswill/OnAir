// EDACS message layer: fixed words assembled by hand from the published bit positions (TSB-69.3 as read in DSD-FME's edacs-fme.c; Extended Addressing from
// US 7,546,135 and the same source), build / parse round trips of every message type in both layouts, ESK, LCN rules, random words.
#include "dect2/edacs/edacs_msg.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static edacs::Msg roundTrip(const edacs::Msg& m, const edacs::Layout& l) {
    uint32_t a, b;
    edacs::build(m, l, a, b);
    return edacs::parse(a, b, l);
}

int main() {
    edacs::Layout std_ = edacs::defaultLayout(), ea = std_;
    ea.extended = true;
    {   // words assembled independently of the builder
        edacs::Msg m = edacs::parse(0x4907aab, 0x680000, std_);
        CHECK(m.type == edacs::MsgType::GroupAssign && m.digital && !m.emergency && m.lid == 0x1234 && m.lcn == 7 && m.txTrunk && m.gid == 0x2AB, "std voice group assignment (lid %d lcn %d gid %d)", m.lid, m.lcn, m.gid);
        m = edacs::parse(0xf661abc, 0x777, std_);
        CHECK(m.type == edacs::MsgType::IndividualAssign && m.txTrunk && m.lcn == 12 && !m.digital && m.gid == 0x1ABC && m.source == 0x777, "std individual assignment");
        m = edacs::parse(0xfd01651, 0, std_);
        CHECK(m.type == edacs::MsgType::SiteId && m.lcn == 1 && m.priority == 3 && !m.scat && m.failsoft && !m.aux && m.site == 17, "std site id");
        m = edacs::parse(0xf8d3001, 0, std_);
        CHECK(m.type == edacs::MsgType::CallEnd && m.lcn == 20 && m.lid == 0x3001, "std console drop");
        m = edacs::parse(0x192beef, 0x354321, ea);
        CHECK(m.type == edacs::MsgType::GroupAssign && m.digital && m.lcn == 9 && !m.update && m.gid == 0xBEEF && m.txTrunk && m.emergency && m.source == 0x54321, "EA voice grant");
        m = edacs::parse(0xfd05ab7, 0, ea);
        CHECK(m.type == edacs::MsgType::SiteId && m.site == 0xB7 && m.area == 0x55, "EA site id (site %d area %d)", m.site, m.area);
    }
    {   // round trips, Standard / Networked
        edacs::Msg m;
        m.type = edacs::MsgType::GroupAssign; m.lcn = 17; m.gid = 2000; m.lid = 16000; m.emergency = true; m.digital = true; m.txTrunk = true;
        edacs::Msg r = roundTrip(m, std_);
        CHECK(r.type == m.type && r.lcn == 17 && r.gid == 2000 && r.lid == 16000 && r.emergency && r.digital && r.txTrunk, "group assignment");
        m.update = true; m.digital = false; m.emergency = true;
        r = roundTrip(m, std_);
        CHECK(r.type == m.type && r.update && r.lcn == 17 && r.gid == 2000 && r.emergency, "group channel update");
        m = edacs::Msg(); m.type = edacs::MsgType::IndividualAssign; m.lcn = 25; m.gid = 16383; m.source = 12345; m.digital = true;
        r = roundTrip(m, std_);
        CHECK(r.type == m.type && r.lcn == 25 && r.gid == 16383 && r.source == 12345 && r.digital, "individual assignment");
        m = edacs::Msg(); m.type = edacs::MsgType::SiteId; m.site = 31; m.lcn = 4; m.priority = 7; m.failsoft = true; m.scat = true; m.aux = true;
        r = roundTrip(m, std_);
        CHECK(r.type == m.type && r.site == 31 && r.lcn == 4 && r.priority == 7 && r.failsoft && r.scat && r.aux, "site id");
        m = edacs::Msg(); m.type = edacs::MsgType::CallEnd; m.lcn = 9; m.lid = 77;
        r = roundTrip(m, std_);
        CHECK(r.type == m.type && r.lcn == 9, "call end");
        m = edacs::Msg(); m.type = edacs::MsgType::DataAssign; m.lcn = 3; m.port = 42; m.individual = true; m.lid = 9000;
        r = roundTrip(m, std_);
        CHECK(r.type == m.type && r.lcn == 3 && r.port == 42 && r.lid == 9000 && r.individual, "data assignment");
        m = edacs::Msg(); m.type = edacs::MsgType::LoginAck; m.gid = 1500; m.lid = 8000;
        r = roundTrip(m, std_);
        CHECK(r.type == m.type && r.gid == 1500 && r.lid == 8000, "login acknowledge");
        m = edacs::Msg(); m.type = edacs::MsgType::StatusRequest; m.status = 248; m.lid = 5;
        r = roundTrip(m, std_);
        CHECK(r.type == m.type && r.status == 248 && r.lid == 5, "status request");
        m = edacs::Msg(); m.type = edacs::MsgType::AdjacentSite; m.lcn = 6; m.index = 5; m.site = 20;
        r = roundTrip(m, std_);
        CHECK(r.type == m.type && r.lcn == 6 && r.index == 5 && r.site == 20, "adjacent site");
        m = edacs::Msg(); m.type = edacs::MsgType::AllCall; m.lcn = 2; m.lid = 5000; m.digital = true; m.update = true; m.txTrunk = true;
        r = roundTrip(m, std_);
        CHECK(r.type == m.type && r.lcn == 2 && r.digital && r.update && r.txTrunk && r.lid == 5000, "all call (lid %d)", r.lid);
    }
    {   // round trips, Extended Addressing
        edacs::Msg m;
        m.type = edacs::MsgType::GroupAssign; m.lcn = 18; m.gid = 65535; m.source = 1048575; m.emergency = true; m.digital = true; m.txTrunk = true; m.update = true;
        edacs::Msg r = roundTrip(m, ea);
        CHECK(r.type == m.type && r.lcn == 18 && r.gid == 65535 && r.source == 1048575 && r.emergency && r.digital && r.txTrunk && r.update, "EA group");
        m.digital = false;
        r = roundTrip(m, ea);
        CHECK(!r.digital && r.type == m.type, "EA analog group");
        m = edacs::Msg(); m.type = edacs::MsgType::IndividualAssign; m.lcn = 5; m.gid = 1000000; m.source = 999999; m.digital = true;
        r = roundTrip(m, ea);
        CHECK(r.type == m.type && r.lcn == 5 && r.gid == 1000000 && r.source == 999999 && r.digital, "EA individual");
        m = edacs::Msg(); m.type = edacs::MsgType::SiteId; m.site = 255; m.area = 127;
        r = roundTrip(m, ea);
        CHECK(r.type == m.type && r.site == 255 && r.area == 127, "EA site");
        m = edacs::Msg(); m.type = edacs::MsgType::AdjacentSite; m.lcn = 11; m.index = 9; m.site = 200;
        r = roundTrip(m, ea);
        CHECK(r.type == m.type && r.lcn == 11 && r.index == 9 && r.site == 200, "EA adjacent");
    }
    {   // LCN 0 does not exist, LCNs from 26 are status codes; ESK flips the words and back
        edacs::Msg m;
        m.type = edacs::MsgType::GroupAssign; m.lcn = 0; m.gid = 5;
        CHECK(roundTrip(m, std_).type == edacs::MsgType::Unknown, "LCN 0 accepted");
        m.lcn = 28;
        CHECK(roundTrip(m, std_).type == edacs::MsgType::Unknown, "LCN 28 (status) taken as a channel");
        m.lcn = 12; m.lid = 33; m.type = edacs::MsgType::GroupAssign;
        edacs::Layout e = std_; e.esk = 0xA0;
        uint32_t a, b, a2, b2;
        edacs::build(m, std_, a, b); edacs::build(m, e, a2, b2);
        CHECK(a2 == (a ^ 0xA000000u) && b2 == (b ^ 0xA000000u), "ESK mask");
        CHECK(edacs::parse(a2, b2, e).gid == m.gid && edacs::parse(a2, b2, e).lcn == 12, "ESK round trip");
        CHECK(edacs::parse(a2, b2, std_).gid != m.gid || edacs::parse(a2, b2, std_).type != edacs::MsgType::GroupAssign, "ESK word read without ESK as the same call");
    }
    {   // random words: nothing crashes, a fair share means nothing
        std::mt19937 rng(3);
        int known = 0;
        for (int i = 0; i < 20000; i++) known += edacs::parse((uint32_t)rng() & 0xFFFFFFF, (uint32_t)rng() & 0xFFFFFFF, std_).type != edacs::MsgType::Unknown;
        CHECK(known > 3000 && known < 18000, "%d of 20000 random words interpreted", known);
        for (int i = 0; i < 20000; i++) known += edacs::parse((uint32_t)rng() & 0xFFFFFFF, (uint32_t)rng() & 0xFFFFFFF, ea).type != edacs::MsgType::Unknown;
        (void)edacs::describe(edacs::parse(0xFFFFFFF, 0xFFFFFFF, std_));
    }
    CHECK(std::string(edacs::describe(roundTrip([] { edacs::Msg m; m.type = edacs::MsgType::GroupAssign; m.lcn = 3; m.gid = 77; m.lid = 10; return m; }(), std_))) == "group call, LCN 3, group 77, unit 10", "describe");
    return testSummary("test_edacs_msg");
}
