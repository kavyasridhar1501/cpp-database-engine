#include "cli/shell.h"

#include <array>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

namespace dbengine {

namespace {

std::vector<std::string> Tokenize(const std::string& line) {
  std::istringstream iss(line);
  std::vector<std::string> tokens;
  std::string tok;
  while (iss >> tok) {
    tokens.push_back(tok);
  }
  return tokens;
}

void PrintHelp(std::ostream& out) {
  out << "Commands:\n"
      << "  alloc                    allocate a new page, prints its id\n"
      << "  write <page_id> <text>   write text (padded/truncated to a page) to page_id\n"
      << "  read <page_id>           read page_id and print it as text\n"
      << "  stats                    print page count / read / write counters\n"
      << "  help                     show this message\n"
      << "  exit                     quit the shell\n";
}

}  // namespace

bool Shell::ExecuteLine(const std::string& line, std::ostream& out) {
  auto tokens = Tokenize(line);
  if (tokens.empty()) {
    return true;
  }

  const std::string& cmd = tokens[0];

  if (cmd == "exit" || cmd == "quit") {
    return false;
  }

  if (cmd == "help") {
    PrintHelp(out);
    return true;
  }

  if (cmd == "alloc") {
    page_id_t page_id = disk_manager_.AllocatePage();
    out << "allocated page " << page_id << "\n";
    return true;
  }

  if (cmd == "write") {
    if (tokens.size() < 3) {
      out << "usage: write <page_id> <text>\n";
      return true;
    }
    page_id_t page_id = std::stoll(tokens[1]);
    // Reconstruct the text from the remaining tokens (allow spaces).
    std::string text = line.substr(line.find(tokens[2]));

    std::array<char, PAGE_SIZE> buf{};
    size_t len = std::min(text.size(), PAGE_SIZE);
    std::memcpy(buf.data(), text.data(), len);
    disk_manager_.WritePage(page_id, buf.data());
    out << "wrote " << len << " bytes to page " << page_id << "\n";
    return true;
  }

  if (cmd == "read") {
    if (tokens.size() < 2) {
      out << "usage: read <page_id>\n";
      return true;
    }
    page_id_t page_id = std::stoll(tokens[1]);
    std::array<char, PAGE_SIZE> buf{};
    disk_manager_.ReadPage(page_id, buf.data());
    // Print up to the first NUL byte, page is otherwise zero-padded.
    out << "page " << page_id << ": " << buf.data() << "\n";
    return true;
  }

  if (cmd == "stats") {
    out << "pages allocated: " << disk_manager_.GetNumPages() << "\n"
        << "disk reads:      " << disk_manager_.GetNumReads() << "\n"
        << "disk writes:     " << disk_manager_.GetNumWrites() << "\n";
    return true;
  }

  out << "unknown command '" << cmd << "', type 'help' for a list\n";
  return true;
}

void Shell::Run() {
  std::cout << "dbengine CLI (Phase 0: raw page access). Type 'help' for commands.\n";
  std::string line;
  while (true) {
    std::cout << "db> ";
    if (!std::getline(std::cin, line)) {
      std::cout << "\n";
      break;
    }
    if (!ExecuteLine(line, std::cout)) {
      break;
    }
  }
}

}  // namespace dbengine
