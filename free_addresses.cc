// load_unload.cc
#include <config.h>
#include <config/command_mgr.h>
#include <hooks/hooks.h>
#include <dhcpsrv/cfgmgr.h>
#include <cc/command_interpreter.h>
#include <process/daemon.h>
#include <cc/data.h>
#include <config/cmds_impl.h>
#include <dhcpsrv/host_mgr.h>
#include <util/boost_time_utils.h>
#include <dhcpsrv/lease_mgr_factory.h>
#include <stats/stats_mgr.h>
#include <algorithm>
#include <dhcpsrv/lease.h>

using namespace isc::dhcp;
using namespace isc::hooks;
using namespace isc::data;
using namespace isc::process;
using namespace isc::config;
using namespace isc::util;
using namespace isc::stats;


ElementPtr createResultSet(const ElementPtr &result_wrapper,
			   const std::vector<std::string>& column_labels) {
    // Create the result-set map and add it to the wrapper.
    ElementPtr result_set = Element::createMap();
    result_wrapper->set("result-set", result_set);

    // Create the timestamp based on time now and add it to the result set.
    boost::posix_time::ptime now(boost::posix_time::microsec_clock::universal_time());

    ElementPtr timestamp = Element::create(isc::util::ptimeToText(now));
    result_set->set("timestamp", timestamp);

    // Create the list of column names and add it to the result set.
    ElementPtr columns = Element::createList();
    for (auto label = column_labels.begin(); label != column_labels.end(); ++label) {
        columns->add(Element::create(*label));
    }
    result_set->set("columns", columns);

    // Create the empty value_rows list, add it and then return it.
    ElementPtr value_rows = Element::createList();
    result_set->set("rows", value_rows);
    return (value_rows);
}


int handleCallout(CalloutHandle& handle) {
  ConstElementPtr response;
  ElementPtr result = Element::createMap();
  std::stringstream os;

  // Set response's result-set labels
  std::vector<std::string>column_labels = { "subnet-id", "free-addresses" };
  ElementPtr value_rows = createResultSet(result, column_labels);

  // Get all subnets
  const Subnet4Collection* subnets =
    CfgMgr::instance().getCurrentCfg()->getCfgSubnets4()->getAll();
  const auto& idx = subnets->get<SubnetSubnetIdIndexTag>();
  auto lower = idx.begin();
  auto upper = idx.end();

  ConstHostCollection hosts;
  int count = 0;
  // Iterate over subnets, find the number of free addresses
  for (auto cur_subnet = lower; cur_subnet != upper; ++cur_subnet) {
    SubnetID cur_id = (*cur_subnet)->getID();

    // Get reservations
    hosts = HostMgr::instance().getAll4(cur_id);

    // Get total addresses
    auto stat = StatsMgr::instance().
      getObservation(StatsMgr::generateName("subnet", cur_id, "total-addresses"));
    auto total = stat->getInteger().first;

    // Remove reservations outside of any address pool
    auto end = std::remove_if(hosts.begin(), hosts.end(), [&] (ConstHostPtr host) {
      return !(*cur_subnet)->inPool(Lease::TYPE_V4, host->getIPv4Reservation());
    });

    // Get leases
    Lease4Collection leases = LeaseMgrFactory::instance().getLeases4(cur_id);

    // Number of reservations in a pool + number of leases
    count = std::distance(hosts.begin(), end) + leases.size();

    // For each lease, If it is also a reservation, remove it from the count
    for (auto i : leases) {
      auto it = std::find_if(hosts.begin(), end, [&] (ConstHostPtr host) {
	return i->addr_ == host->getIPv4Reservation();
      });
      if (it != end) {
	count--;
      }
    }

    // Add the current result to the result-set
    ElementPtr row = Element::createList();
    row->add(Element::create(static_cast<int64_t>(cur_id)));
    row->add(Element::create(total - count));
    value_rows->add(row);
  }

  os << "Ok";
  response = createAnswer(CONTROL_RESULT_SUCCESS, os.str(), result);
  handle.setArgument("response", response);
  return 0;
}


extern "C" {

int load(LibraryHandle& handle) {
    const std::string& proc_name = Daemon::getProcName();
    if (proc_name != "kea-dhcp4") {
      isc_throw(isc::Unexpected, "Bad process name: " << proc_name
		<< ", expected kea-dhcp4");
    }

    handle.registerCommandCallout("free-addresses", handleCallout);
    return (0);
}
int unload() {
    return (0);
}
}
