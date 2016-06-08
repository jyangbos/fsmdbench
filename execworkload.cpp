#include <string>
#include <iostream>
#include <fstream>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include "barrier.h"
#include "nameset.h"
#include "namegen.h"
#include "processopts.h"
#include "execworkload.h"

exec_workload::exec_workload(std::string top, int nl, std::vector<std::string> *addrs, int local_idx, int dirs, int files, int nclient, std::string out)
  : target_dir(top), nlevel(nl), addresses(addrs), local_index(local_idx), dirs_per_client(dirs), files_per_client(files), num_client_threads(nclient), output_path(out)
{
  comm_channel = new conn_channel();
  comm_channel->init(local_idx, addrs);
  door = new barrier(2, num_client_threads*addresses->size(), comm_channel);
}

exec_workload::~exec_workload()
{
  delete door;
  delete comm_channel;
}

void exec_workload::handle_one_dir(int op, std::vector<std::string>& names)
{
  handle_one(op, &dop, names, &data_d);
}

void exec_workload::handle_one_file(int op, std::vector<std::string>& names)
{
  handle_one(op, &fop, names, &data_f);
}

void exec_workload::handle_one(int op, timed_task *task, std::vector<std::string>& names, stats *data_handler)
{
  task_result result;
  for (auto& dn : names)
    {
      task->exec(op, &dn, result);
      data_handler->process((double)result.duration, result.status_code != 0);
      if (data_file.is_open())
        {
          data_file << "op " << get_op_name(op) << " status " << result.status_code << " in " << result.duration << "(microsec) on " << dn << std::endl;
        }
    }
}

void exec_workload::print_summary(int op, std::string object_type, stats& data)
{
  std::cout << get_op_name(op) << " " << object_type << " result:" << std::endl
            << "   throughput(ops/s):  " << std::to_string(data.ops()) << std::endl
            << "   latency (micro sec) " << std::endl
            << "              average: " << std::to_string(data.average()) << std::endl
            << "             variance: " << std::to_string(data.variance()) << std::endl
            << "              maximum: " << std::to_string(data.maxvalue()) << std::endl
            << "   errors: " << std::to_string(data.errors()) << std::endl
            << std::endl;
}

void exec_workload::measure_op_oneclient(int op, int initial_level, int incr, std::vector<name_set> *nameset, int client_thread_index)
{
  if (0 == client_thread_index)
    {
      data_d.reset();
      data_f.reset();
    }
  for (int l = initial_level; l >= 0 && l < nlevel; l += incr)
    {
      door->notify_and_wait(1);
      handle_one_dir(op, (*nameset)[l].get_dir_names());
      handle_one_file(op, (*nameset)[l].get_file_names());
      door->notify_and_wait(2);
    }
  if (0 == client_thread_index)
    {
      print_summary(op, std::string("directories"), data_d);
      print_summary(op, std::string("files"), data_f);
    }
}

void exec_workload::bench_oneclient(exec_workload *ew, const int client_thread_index)
{
  int client_index = ew->get_client_index(client_thread_index);
  int total_clients = ew->num_client_threads * ew->addresses->size();

  name_generator namegen(ew->target_dir, std::string("d"), std::string("f"), client_index, total_clients, ew->nlevel, ew->dirs_per_client, ew->files_per_client);
  std::vector<name_set> nameset(ew->nlevel);
  for (int l = 0; l < ew->nlevel; l++)
    {
      if (0 != namegen.next_level(nameset[l]))
        {
          return;
        }
    }

  ew->measure_op_oneclient(CREATE, 0, 1, &nameset, client_thread_index);
  ew->measure_op_oneclient(UPDATE, 0, 1, &nameset, client_thread_index);
  ew->measure_op_oneclient(DELETE, ew->nlevel-1, -1, &nameset, client_thread_index);

  return;
}

void exec_workload::exec_benchmark()
{
  data_file.open(output_path);

  std::cout << std::endl;
  
  for (int i = 0; i < num_client_threads; i++)
    {
      client_threads.push_back(std::thread(bench_oneclient, this, i));
    }

  for (int i = 0; i < num_client_threads; i++)
    {
      client_threads[i].join();
    }

  if (data_file.is_open())
    {
      data_file.close();
    }
}

int exec_workload::get_client_index(const int client_thread_index)
{
  return client_thread_index + local_index*num_client_threads;
}
