/*  =========================================================================
    persistance - Some helper functions for persistance layer

    Copyright (C) 2014 - 2015 Eaton                                        
                                                                           
    This program is free software; you can redistribute it and/or modify   
    it under the terms of the GNU General Public License as published by   
    the Free Software Foundation; either version 2 of the License, or      
    (at your option) any later version.                                    
                                                                           
    This program is distributed in the hope that it will be useful,        
    but WITHOUT ANY WARRANTY; without even the implied warranty of         
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          
    GNU General Public License for more details.                           
                                                                           
    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.            
    =========================================================================
*/

/*
@header
    persistance - Some helper functions for persistance layer
@discuss
@end
*/

#include "agent_metric_store_classes.h"


int
select_topic (
        const std::string &connurl,
        const std::string &topic, // the whole topic XXX@YYY
        std::function<void(
                        const tntdb::Row&)>& cb)
{
    try {
        tntdb::Connection conn = tntdb::connectCached(connurl);

        tntdb::Statement st = conn.prepareCached (
            " SELECT "
            "   * "
            " FROM v_bios_measurement_topic "
            " WHERE "
            "   topic = :topic "
        );

        tntdb::Row row = st.set ("topic", topic)
                                 .selectRow ();

        cb(row);
        return 0;
    }
    catch (const tntdb::NotFound &e) {
        log_error("Topic '%s' not found", topic.c_str());
        return -2;
    }
    catch (const std::exception &e) {
        log_error("Exception caught: %s", e.what());
        return -1;
    }
    catch (...) {
        log_error("Unknown exception caught!");
        return -1;
    }
}



int
select_measurements (
        const std::string &connurl,
        const std::string &topic, // the whole topic XXX@YYY
        int64_t start_timestamp,
        int64_t end_timestamp,
        std::function<void(
                        const tntdb::Row&)>& cb)
{
    try {
        tntdb::Connection conn = tntdb::connectCached(connurl);

        tntdb::Statement st = conn.prepareCached (
            " SELECT "
            "   topic, value, scale, timestamp, units"
            " FROM v_bios_measurement "
            " WHERE "
            "   topic = :topic AND "
            "   timestamp >= :time_st AND "
            "   timestamp <= :time_end"
        );
        // ACE: I know, that topic_id would have better performance, but
        // for first iteration lets stay with this approach
        tntdb::Result result = st.set ("topic", topic)
                                 .set ("time_st", start_timestamp)
                                 .set ("time_end", end_timestamp)
                                 .select ();

        for (const auto &row: result) {
            cb(row);
        }
        return 0;
    }
    catch (const std::exception &e) {
        log_error("Exception caught: %s", e.what());
        return -1;
    }
    catch (...) {
        log_error("Unknown exception caught!");
        return -1;
    }
}

m_dvc_id_t
    insert_as_not_classified_device(
        tntdb::Connection &conn,
        const char        *device_name)
{
    if(device_name == NULL || device_name[0] == 0 ){
        log_error("[t_bios_discovered_device] can't insert a device with NULL or non device name");
        return 0;
    }
    m_dvc_id_t id_discovered_device=0;
    tntdb::Statement st;
    st = conn.prepareCached(
        " INSERT INTO"
        "   t_bios_discovered_device"
        "     (name, id_device_type)"
        " SELECT"
        "   :name,"
        "   (SELECT T.id_device_type FROM t_bios_device_type T WHERE T.name = 'not_classified')"
        " FROM"
        "   ( SELECT NULL name, 0 id_device_type ) tbl"
        " WHERE :name NOT IN (SELECT name FROM t_bios_discovered_device )"
     );
    uint32_t n = st.set("name", device_name).execute();
    id_discovered_device=conn.lastInsertId();
    log_debug("[t_discovered_device]: device '%s' inserted %" PRIu32 " rows ",
              device_name, n);
    if( n == 1 ) {
        // try to update  relation table
        st = conn.prepareCached(
            " INSERT INTO"
            "   t_bios_monitor_asset_relation (id_discovered_device, id_asset_element)"
            " SELECT"
            "   DD.id_discovered_device, AE.id_asset_element"
            " FROM"
            "   t_bios_discovered_device DD INNER JOIN t_bios_asset_element AE on DD.name = AE.name"
            " WHERE"
            "   DD.name = :name AND"
            "   DD.id_discovered_device NOT IN ( SELECT id_discovered_device FROM t_bios_monitor_asset_relation )"
        );
        n = st.set("name", device_name).execute();
        log_debug("[t_bios_monitor_asset_relation]: inserted %" PRIu32 " rows about %s", n, device_name);
    }else{
        log_error("[t_discovered_device]:  device %s not inserted", device_name );
    }
    return id_discovered_device;
}


//return id_discovered_device or 0 in case of issue
m_dvc_id_t
    prepare_discovered_device(
        tntdb::Connection &conn,
        const char        *device_name)
{
    assert (device_name);

    // verify if the device name exists in t_bios_discovered_device
    // if not create it as not_classified device type
    m_dvc_id_t id_discovered_device = 0;
    tntdb::Statement st = conn.prepareCached(
            " SELECT id_discovered_device "
            " FROM "
            "    t_bios_discovered_device v"
            " WHERE "
            "   v.name = :name");
    st.set("name", device_name);
    try{
        tntdb::Row row=st.selectRow();
        row["id_discovered_device"].get(id_discovered_device);
        return id_discovered_device;
    }catch(const tntdb::NotFound &e){
        log_debug("[t_bios_discovered_device] device %s not found => try to create it as not classified",device_name);
        //add the device as 'not_classified' type
        // probably device doesn't exist in t_bios_discovered_device. Let's fill it.
        return insert_as_not_classified_device(conn,device_name);
    }
}


//return topic_id or 0 in case of issue
m_msrmnt_tpc_id_t
    prepare_topic(
        tntdb::Connection &conn,
        const char        *topic,
        const char        *units,
        const char        *device_name)
{
    assert ( topic );
    assert ( units );
    assert ( device_name );

    m_dvc_id_t id_discovered_device = prepare_discovered_device (conn, device_name);
    if ( id_discovered_device == 0 ) {
        return 0;
    }
    try {
        tntdb::Statement st =  conn.prepareCached(
            " INSERT INTO "
            "   t_bios_measurement_topic "
            "    (topic, units, device_id) "
            " VALUES (:topic, :units, :device_id) "
            " ON DUPLICATE KEY "
            "   UPDATE "
            "      id = LAST_INSERT_ID(id) "
        );

        uint32_t n = st.set("topic", topic)
                       .set("units", units)
                       .set("device_id", id_discovered_device)
                       .execute();

        m_msrmnt_tpc_id_t topic_id = conn.lastInsertId();
        if ( topic_id != 0 ) {
            log_debug("[t_bios_measurement_topic]: inserted topic %s, #%" PRIu32 " rows , topic_id %u", topic, n, topic_id);
        } else {
            log_error("[t_bios_measurement_topic]:  topic %s not inserted", topic);
        }
        return topic_id;
    } catch(const std::exception &e) {
        log_error ("Topic '%s' was not inserted with error: %s", topic, e.what());
        return 0;
    }
}


int
    insert_into_measurement(
        tntdb::Connection &conn,
        const char        *topic,
        m_msrmnt_value_t   value,
        m_msrmnt_scale_t   scale,
        int64_t            time,
        const char        *units,
        const char        *device_name)
{
    assert ( units );
    assert ( device_name );
    assert ( topic );

    if ( topic[0]=='@' ) {
        log_error ("malformed value of topic '%s' is not allowed", topic);
        return 1;
    }

    try {
        m_msrmnt_tpc_id_t topic_id = prepare_topic(conn, topic, units, device_name);
        if ( topic_id == 0 ) {
            // topic was not inserted -> cannot insert metric
            return 1;
        }
        tntdb::Statement st = conn.prepareCached (
                "INSERT INTO t_bios_measurement (timestamp, value, scale, topic_id) "
                "  VALUES (:time, :value, :scale, :topic_id)"
                "  ON DUPLICATE KEY UPDATE value = :value, scale = :scale");
        st.set("time",  time)
            .set("value", value)
            .set("scale", scale)
            .set("topic_id", topic_id)
            .execute();

        log_debug("[t_bios_measurement]: inserted " \
                "value:%" PRIi32 " * 10^%" PRIi16 " %s "\
                "topic = '%s' topic_id=%" PRIi16 " time = %" PRIu64,
                value, scale, units, topic, topic_id, time);
        return 0;
    } catch(const std::exception &e) {
        log_error ("Metric with topic '%s' was not inserted with error: %s", topic, e.what());
        return 1;
    }
}


//  --------------------------------------------------------------------------
//  Self test of this class

void
persistance_test (bool verbose)
{
    printf (" * persistance: ");
    //  @selftest
    //  @end
    printf ("OK\n");
}