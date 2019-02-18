/* ---------------------------------------------------------------------
 * Numenta Platform for Intelligent Computing (NuPIC)
 * Copyright (C) 2013-2017, Numenta, Inc.  Unless you have an agreement
 * with Numenta, Inc., for a separate license for this software code, the
 * following terms and conditions apply:
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Affero Public License for more details.
 *
 * You should have received a copy of the GNU Affero Public License
 * along with this program.  If not, see http://www.gnu.org/licenses.
 *
 * http://numenta.org/licenses/
 * ---------------------------------------------------------------------
 */

/** @file
 * Implementation of the Link class
 */
#include <cstring> // memcpy,memset
#include <nupic/engine/Input.hpp>
#include <nupic/engine/Link.hpp>
#include <nupic/engine/Output.hpp>
#include <nupic/engine/Region.hpp>
#include <nupic/ntypes/Array.hpp>
#include <nupic/types/BasicType.hpp>
#include <nupic/utils/Log.hpp>

// Set this to true when debugging to enable handy debug-level logging of data
// moving through the links, including the delayed link transitions.
#define _LINK_DEBUG false

namespace nupic {


Link::Link(const std::string &linkType, const std::string &linkParams,
           const std::string &srcRegionName, const std::string &destRegionName,
           const std::string &srcOutputName, const std::string &destInputName,
           const size_t propagationDelay) {
  commonConstructorInit_(linkType, linkParams, srcRegionName, destRegionName,
                         srcOutputName, destInputName, propagationDelay);
}

Link::Link(const std::string &linkType, const std::string &linkParams,
           Output *srcOutput, Input *destInput, const size_t propagationDelay) {
  commonConstructorInit_(linkType, linkParams, srcOutput->getRegion()->getName(),
                         destInput->getRegion()->getName(), srcOutput->getName(),
                         destInput->getName(), propagationDelay);

  connectToNetwork(srcOutput, destInput);
  // Note -- link is not usable until we set the destOffset, which happens at
  // initialization time
}

Link::Link() {  // needed for deserialization
  destOffset_ = 0;
  src_ = nullptr;
  dest_ = nullptr;
  initialized_ = false;
}

void Link::commonConstructorInit_(const std::string &linkType,
                                  const std::string &linkParams,
                                  const std::string &srcRegionName,
                                  const std::string &destRegionName,
                                  const std::string &srcOutputName,
                                  const std::string &destInputName,
                                  const size_t propagationDelay) {
  linkType_ = linkType;
  linkParams_ = linkParams;
  srcRegionName_ = srcRegionName;
  srcOutputName_ = srcOutputName;
  destRegionName_ = destRegionName;
  destInputName_ = destInputName;
  propagationDelay_ = propagationDelay;
  destOffset_ = 0;
  is_FanIn_ = false;
  src_ = nullptr;
  dest_ = nullptr;
  initialized_ = false;

}




void Link::initialize(size_t destinationOffset, bool is_FanIn) {
  // Make sure all information is specified and
  // consistent. Unless there is a NuPIC implementation
  // error, all these checks are guaranteed to pass
  // because of the way the network is constructed
  // and initialized.

  // Make sure we have been attached to a real network
  NTA_CHECK(src_ != nullptr)
      << "Link::initialize() and src_ Output object not set.";
  NTA_CHECK(dest_ != nullptr)
      << "Link::initialize() and dest_ Input object not set.";

  // Confirm that our dimensions are consistent with the
  // dimensions of the regions we're connecting.
  const Dimensions &srcD = getSrcDimensions();
  const Dimensions &destD = getDestDimensions();
  NTA_CHECK(!srcD.isUnspecified());
  NTA_CHECK(!destD.isUnspecified());

  Dimensions oneD;
  oneD.push_back(1);

  if (src_->isRegionLevel()) {
    Dimensions d;
    for (size_t i = 0; i < src_->getRegion()->getDimensions().size(); i++) {
      d.push_back(1);
    }

    NTA_CHECK(srcD.isDontcare() || srcD == d);
  } else if (src_->getRegion()->getDimensions() == oneD) {
    Dimensions d;
    for (size_t i = 0; i < srcD.size(); i++) {
      d.push_back(1);
    }
    NTA_CHECK(srcD.isDontcare() || srcD == d);
  } else {
    NTA_CHECK(srcD.isDontcare() || srcD == src_->getRegion()->getDimensions());
  }

  if (dest_->isRegionLevel()) {
    Dimensions d;
    for (size_t i = 0; i < dest_->getRegion()->getDimensions().size(); i++) {
      d.push_back(1);
    }

    NTA_CHECK(destD.isDontcare() || destD.isOnes());
  } else if (dest_->getRegion()->getDimensions() == oneD) {
    Dimensions d;
    for (size_t i = 0; i < destD.size(); i++) {
      d.push_back(1);
    }
    NTA_CHECK(destD.isDontcare() || destD == d);
  } else {
    NTA_CHECK(destD.isDontcare() ||
              destD == dest_->getRegion()->getDimensions());
  }


  destOffset_ = destinationOffset;
  is_FanIn_ = is_FanIn;

  // ---
  // Initialize the propagation delay buffer
  // But skip it if it already has something in it from deserialize().
  // ---
  if (propagationDelay_ > 0 && propagationDelayBuffer_.empty()) {
    // Initialize delay data elements.  This must be done during initialize()
    // because the buffer size is not known prior to then.
    // front of queue will be the next value to be copied to the dest Input buffer.
    // back of queue will be the same as the current contents of source Output.
    Array &output_buffer = src_->getData();
    for (size_t i = 0; i < (propagationDelay_); i++) {
      Array delayedbuffer = output_buffer.copy();
      delayedbuffer.zeroBuffer();
      propagationDelayBuffer_.push_back(delayedbuffer);
    }
  }

  initialized_ = true;
}

void Link::setSrcDimensions(Dimensions &dims) {
  NTA_CHECK(src_ != nullptr && dest_ != nullptr)
      << "Link::setSrcDimensions() can only be called on a connected link";

  elementCount_ = src_->getNodeOutputElementCount();
  if (elementCount_ == 0) {
    elementCount_ =
        src_->getRegion()->getNodeOutputElementCount(src_->getName());
  }
   srcDimensions_ = dims;
   if (destDimensions_.isUnspecified())
     destDimensions_ = dims;
}

void Link::setDestDimensions(Dimensions &dims) {
  NTA_CHECK(src_ != nullptr && dest_ != nullptr)
      << "Link::setDestDimensions() can only be called on a connected link";

  elementCount_ = src_->getNodeOutputElementCount();
  if (elementCount_ == 0) {
    elementCount_ =
        src_->getRegion()->getNodeOutputElementCount(src_->getName());
  }

  destDimensions_ = dims;
  if (srcDimensions_.isUnspecified())
    srcDimensions_ = dims;
}

const Dimensions &Link::getSrcDimensions() const {
  return srcDimensions_;
};

const Dimensions &Link::getDestDimensions() const {
  return destDimensions_;
};

// Return constructor params
const std::string &Link::getLinkType() const { return linkType_; }

const std::string &Link::getLinkParams() const { return linkParams_; }

const std::string &Link::getSrcRegionName() const { return srcRegionName_; }

const std::string &Link::getSrcOutputName() const { return srcOutputName_; }

const std::string &Link::getDestRegionName() const { return destRegionName_; }

const std::string &Link::getDestInputName() const { return destInputName_; }

std::string Link::getMoniker() const {
  std::stringstream ss;
  ss << getSrcRegionName() << "." << getSrcOutputName() << "-->"
     << getDestRegionName() << "." << getDestInputName();
  return ss.str();
}

const std::string Link::toString() const {
  std::stringstream ss;
  ss << "[" << getSrcRegionName() << "." << getSrcOutputName();
  if (src_) {
    ss << " (region dims: " << src_->getRegion()->getDimensions().toString()
       << ") ";
  }
  ss << " to " << getDestRegionName() << "." << getDestInputName();
  if (dest_) {
    ss << " (region dims: " << dest_->getRegion()->getDimensions().toString()
       << ") ";
  }
  ss << " type: " << linkType_ << "]";
  return ss.str();
}

void Link::connectToNetwork(Output *src, Input *dest) {
  NTA_CHECK(src != nullptr);
  NTA_CHECK(dest != nullptr);

  src_ = src;
  dest_ = dest;
}

// The methods below only work on connected links.
Output &Link::getSrc() const

{
  NTA_CHECK(src_ != nullptr)
      << "Link::getSrc() can only be called on a connected link";
  return *src_;
}

Input &Link::getDest() const {
  NTA_CHECK(dest_ != nullptr)
      << "Link::getDest() can only be called on a connected link";
  return *dest_;
}


void Link::compute() {
  NTA_CHECK(initialized_);

  if (propagationDelay_) {
    // A delayed link's queue buffer size should always be number of delays.
    NTA_CHECK(propagationDelayBuffer_.size() == (propagationDelay_));
  }

  // Copy data from source to destination. For delayed links, will copy from
  // head of circular queue; otherwise directly from source.
  const Array &src = propagationDelay_ ? propagationDelayBuffer_.front() : src_->getData();

  Array &dest = dest_->getData();

  if (_LINK_DEBUG) {
    NTA_DEBUG << "Link::compute: " << getMoniker() << "; copying to dest input"
              << "; delay=" << propagationDelay_ << "; size=" << src.getCount()
              << " type=" << BasicType::getName(src.getType())
              << " --> " << BasicType::getName(dest.getType()) << std::endl;
  }

	NTA_CHECK(src.getCount() + destOffset_ <= dest.getMaxElementsCount())
        << "Not enough room in buffer to propogate to " << destRegionName_
        << " " << destInputName_ << ". ";

  if (src.getType() == dest.getType() && !is_FanIn_)
    dest = src;   // Performs a shallow copy. Data not copied but passed in shared_ptr.
  else {
    // we must perform a deep copy with possible type conversion.
    // It is copied into the destination Input
    // buffer at the specified offset so an Input with multiple incoming links
    // has the Output buffers appended into a single large Input buffer.
    src.convertInto(dest, destOffset_, dest.getMaxElementsCount());
  }
}

void Link::shiftBufferedData() {
  if (propagationDelay_) {   // Source buffering is not used in 0-delay links
    Array *from = &src_->getData();
    NTA_CHECK(propagationDelayBuffer_.size() == (propagationDelay_));

    // push a copy of the source Output buffer on the back of the queue.
    // This must be a deep copy.
    Array a = from->copy();
    propagationDelayBuffer_.push_back(a);

    // Pop the head of the queue
    // The top of the queue now becomes the value to copy to destination.
    propagationDelayBuffer_.pop_front();
  }
}

void Link::serialize(std::ostream &f) {
  size_t srcCount = ((!src_) ? (size_t)0 : src_->getData().getCount());

  f << "{\n";
  f << "linkType: " <<  getLinkType() << "\n";
  f << "params: " << getLinkParams() << "\n";
  f << "srcRegion: " << getSrcRegionName() << "\n";
  f << "srcOutput: " << getSrcOutputName() << "\n";
  f << "destRegion: " << getDestRegionName() << "\n";
  f << "destInput: " << getDestInputName() << "\n";
  f << "propagationDelay: " << propagationDelay_ << "\n";
  f << "propagationDelayBuffer: [ " << propagationDelayBuffer_.size() << "\n";
  if (propagationDelay_ > 0) {
    // we need to capture the propagationDelayBuffer_ used for propagationDelay
    // Do not copy the last entry.  It is the same as the output buffer.

    // The current contents of the Destination Input buffer also needs
    // to be captured as if it were the top value of the propagationDelayBuffer.
    // When restored, it will be copied to the dest input buffer and popped off
    // before the next execution. If there is an offset, we only
    // want to capture the amount of the input buffer contributed by
    // this link.
    Array a = dest_->getData().subset(destOffset_, srcCount);
    a.save(f); // our part of the current Dest Input buffer.

    std::deque<Array>::iterator itr;
    for (auto itr = propagationDelayBuffer_.begin();
         itr != propagationDelayBuffer_.end(); itr++) {
      if (itr + 1 == propagationDelayBuffer_.end())
        break; // skip the last buffer. Its the current output.
      Array &buf = *itr;
      buf.save(f);
    } // end for
  }
  f << "]\n";  // end of list of buffers in propagationDelayBuffer

  f << "}\n";  // end of sequence
}

void Link::deserialize(std::istream &f) {
  // Each link is a map -- extract the 9 values in the map
  // The "circularBuffer" element is a two dimentional array only present if
  // propogationDelay > 0.
  char bigbuffer[5000];
  std::string tag;
  Size count;
  std::string linkType;
  std::string linkParams;
  std::string srcRegionName;
  std::string srcOutputName;
  std::string destRegionName;
  std::string destInputName;
  Size propagationDelay;

  f >> tag;
  NTA_CHECK(tag == "{") << "Invalid network structure file -- bad link (not a map)";

  // 1. type
  f >> tag;
  NTA_CHECK(tag == "linkType:");
  f.ignore(1);
  f.getline(bigbuffer, sizeof(bigbuffer));
  linkType = bigbuffer;

  // 2. params
  f >> tag;
  NTA_CHECK(tag == "params:");
  f.ignore(1);
  f.getline(bigbuffer, sizeof(bigbuffer));
  linkParams = bigbuffer;

  // 3. srcRegion (name)
  f >> tag;
  NTA_CHECK(tag == "srcRegion:");
  f.ignore(1);
  f.getline(bigbuffer, sizeof(bigbuffer));
  srcRegionName = bigbuffer;

  // 4. srcOutput
  f >> tag;
  NTA_CHECK(tag == "srcOutput:");
  f.ignore(1);
  f.getline(bigbuffer, sizeof(bigbuffer));
  srcOutputName = bigbuffer;

  // 5. destRegion
  f >> tag;
  NTA_CHECK(tag == "destRegion:");
  f.ignore(1);
  f.getline(bigbuffer, sizeof(bigbuffer));
  destRegionName = bigbuffer;

  // 6. destInput
  f >> tag;
  NTA_CHECK(tag == "destInput:");
  f.ignore(1);
  f.getline(bigbuffer, sizeof(bigbuffer));
  destInputName = bigbuffer;

  // 7. propagationDelay (number of cycles to delay propagation)
  f >> tag;
  NTA_CHECK(tag == "propagationDelay:");
  f >> propagationDelay;

  // fill in the data for the Link object
  commonConstructorInit_(linkType, linkParams, srcRegionName, destRegionName,
                         srcOutputName, destInputName, propagationDelay);

  // 8. propagationDelayBuffer
  f >> tag;
  NTA_CHECK(tag == "propagationDelayBuffer:");
  f >> tag;
  NTA_CHECK(tag == "[")  << "Expected start of a sequence.";
  f >> count;
  f.ignore(1);
  // if no propagationDelay (value = 0) then there should be an empty sequence.
  NTA_CHECK(count == propagationDelay_) << "Invalid network structure file -- "
            "link has " << count << " buffers in 'propagationDelayBuffer'. "
            << "Expecting " << propagationDelay << ".";
  Size idx = 0;
  for (; idx < count; idx++) {
    Array a;
    a.load(f);
    propagationDelayBuffer_.push_back(a);
  }
  // To complete the restore, call r->prepareInputs() and then shiftBufferedData();
  // This is performed in Network class at the end of the load().
  f >> tag;
  NTA_CHECK(tag == "]");
  f >> tag;
  NTA_CHECK(tag == "}");
  f.ignore(1);
}



bool Link::operator==(const Link &o) const {
  if (initialized_ != o.initialized_ ||
      propagationDelay_ != o.propagationDelay_ || linkType_ != o.linkType_ ||
      linkParams_ != o.linkParams_ || destOffset_ != o.destOffset_ ||
      srcRegionName_ != o.srcRegionName_ ||
      destRegionName_ != o.destRegionName_ ||
      srcOutputName_ != o.srcOutputName_ ||
      destInputName_ != o.destInputName_) {
    return false;
	// TODO: compare propagationDelayBuffer
  }
  return true;
}


/**
 * A readable display of a Link.
 * This is not part of the save/load facility.
 */
std::ostream &operator<<(std::ostream &f, const Link &link) {
  f << "<Link>\n";
  f << "  <type>" << link.getLinkType() << "</type>\n";
  f << "  <params>" << link.getLinkParams() << "</params>\n";
  f << "  <srcRegion>" << link.getSrcRegionName() << "</srcRegion>\n";
  f << "  <destRegion>" << link.getDestRegionName() << "</destRegion>\n";
  f << "  <srcOutput>" << link.getSrcOutputName() << "</srcOutput>\n";
  f << "  <destInput>" << link.getDestInputName() << "</destInput>\n";
  f << "  <propagationDelay>" << link.getPropagationDelay()
    << "</propagationDelay>\n";
  if (link.getPropagationDelay() > 0) {
  	f <<   "   <propagationDelayBuffer>\n";
	for (auto buf : link.propagationDelayBuffer_) {
		f << buf << "\n";
	}
	f <<   "   </propagationDelayBuffer>\n";
  }
  f << "</Link>\n";
  return f;
}

} // namespace nupic
