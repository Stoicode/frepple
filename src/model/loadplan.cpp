/***************************************************************************
 *                                                                         *
 * Copyright (C) 2007-2015 by frePPLe bvba                                 *
 *                                                                         *
 * This library is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU Affero General Public License as published   *
 * by the Free Software Foundation; either version 3 of the License, or    *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This library is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
 * GNU Affero General Public License for more details.                     *
 *                                                                         *
 * You should have received a copy of the GNU Affero General Public        *
 * License along with this program.                                        *
 * If not, see <http://www.gnu.org/licenses/>.                             *
 *                                                                         *
 ***************************************************************************/

#define FREPPLE_CORE
#include "frepple/model.h"

namespace frepple {

const MetaCategory* LoadPlan::metacategory;
const MetaClass* LoadPlan::metadata;

int LoadPlan::initialize() {
  // Initialize the metadata
  metacategory =
      MetaCategory::registerCategory<LoadPlan>("loadplan", "loadplans", reader);
  registerFields<LoadPlan>(const_cast<MetaCategory*>(metacategory));
  metadata = MetaClass::registerClass<LoadPlan>("loadplan", "loadplan");

  // Initialize the Python type
  PythonType& x = FreppleCategory<LoadPlan>::getPythonType();
  x.setName("loadplan");
  x.setDoc("frePPLe loadplan");
  x.supportgetattro();
  x.supportsetattro();
  x.supportcreate(create);
  const_cast<MetaClass*>(metadata)->pythonClass = x.type_object();
  return x.typeReady();
}

LoadPlan::LoadPlan(OperationPlan* o, const Load* r) {
  // Initialize the Python type
  initType(metadata);

  assert(o);
  ld = const_cast<Load*>(r);
  oper = o;

  // Update the resource field
  res = r->findPreferredResource(o->getSetupEnd());

  // Add to the operationplan
  nextLoadPlan = nullptr;
  if (o->firstloadplan) {
    // Append to the end
    LoadPlan* c = o->firstloadplan;
    while (c->nextLoadPlan) c = c->nextLoadPlan;
    c->nextLoadPlan = this;
  } else
    // First in the list
    o->firstloadplan = this;

  // Insert in the resource timeline
  getResource()->loadplans.insert(this, ld->getLoadplanQuantity(this),
                                  ld->getLoadplanDate(this));

  // For continuous resources, create a loadplan to mark
  // the end of the operationplan.
  if (!getResource()->hasType<ResourceBuckets>()) new LoadPlan(o, r, this);

  // Mark the operation and resource as being changed. This will trigger
  // the recomputation of their problems
  getResource()->setChanged();
  r->getOperation()->setChanged();
}

LoadPlan::LoadPlan(OperationPlan* o, const Load* r, LoadPlan* lp) {
  ld = const_cast<Load*>(r);
  oper = o;
  flags |= TYPE_END;

  // Update the resource field
  res = lp->getResource();

  // Add to the operationplan
  nextLoadPlan = nullptr;
  if (o->firstloadplan) {
    // Append to the end
    LoadPlan* c = o->firstloadplan;
    while (c->nextLoadPlan) c = c->nextLoadPlan;
    c->nextLoadPlan = this;
  } else
    // First in the list
    o->firstloadplan = this;

  // Insert in the resource timeline
  getResource()->loadplans.insert(this, ld->getLoadplanQuantity(this),
                                  ld->getLoadplanDate(this));

  // Initialize the Python type
  initType(metadata);
}

void LoadPlan::setResource(Resource* newres, bool check, bool use_start) {
  // Nothing to do
  if (res == newres) return;

  // Validate the argument
  if (!newres) throw DataException("Can't switch to nullptr resource");
  if (check) {
    // New resource must be a subresource of the load's resource.
    bool ok = false;
    for (const Resource* i = newres; i && !ok; i = i->getOwner())
      if (i == getLoad()->getResource()) ok = true;
    if (!ok)
      throw DataException(
          "Resource isn't matching the resource specified on the load");

    // New resource must have the required skill
    if (getLoad()->getSkill()) {
      ok = false;
      Resource::skilllist::const_iterator s = newres->getSkills();
      while (ResourceSkill* rs = s.next())
        if (rs->getSkill() == getLoad()->getSkill()) {
          ok = true;
          break;
        }
      if (!ok)
        throw DataException("Resource misses the skill specified on the load");
    }
  }

  // Mark entities as changed
  Resource* oldRes = res;
  if (oper) oper->getOperation()->setChanged();
  if (res && res != newres) res->setChanged();
  newres->setChanged();

  // Change this loadplan and its brother
  LoadPlan* ldplan =
      getResource()->hasType<ResourceBuckets>() ? this : getOtherLoadPlan();
  while (ldplan) {
    // Remove from the old resource, if there is one
    if (res) {
      res->loadplans.erase(ldplan);
      res->setChanged();
    }

    // Insert in the new resource.
    // This code assumes the date and quantity of the loadplan don't change
    // when a new resource is assigned.
    ldplan->res = newres;
    newres->loadplans.insert(ldplan, ld->getLoadplanQuantity(ldplan),
                             ld->getLoadplanDate(ldplan));

    // Repeat for the brother loadplan or exit
    if (ldplan != this)
      ldplan = this;
    else
      break;
  }

  // Clear the setup event
  oper->setStartEndAndQuantity(oper->getSetupEnd(), oper->getEnd(),
                               oper->getQuantity());
  oper->clearSetupEvent();

  // The new resource may have a different availability calendar,
  // and we need to make sure to respect it.
  if (use_start)
    oper->setStart(oper->getStart());
  else
    oper->setEnd(oper->getEnd());

  // Update the setup time on the old resource
  if (oldRes) oldRes->updateSetupTime();

  // Change the resource
  newres->setChanged();
}

LoadPlan* LoadPlan::getOtherLoadPlan() const {
  if (getResource()->hasType<ResourceBuckets>()) return nullptr;
  for (auto i = oper->firstloadplan; i; i = i->nextLoadPlan)
    if (i->ld == ld && i != this && i->getEventType() == 1) return i;
  throw LogicException("No matching loadplan found");
}

string LoadPlan::getStatus() const {
  if (flags & STATUS_CONFIRMED)
    return "confirmed";
  else if (flags & STATUS_CLOSED)
    return "closed";
  else
    return "proposed";
}

void LoadPlan::setStatus(const string& s) {
  if (s == "confirmed") {
    if (getOperationPlan()->getProposed())
      throw DataException(
          "OperationPlanResource status change to confirmed while "
          "OperationPlan is proposed");
    setConfirmed(true);
  } else if (s == "proposed")
    setProposed(true);
  else if (s == "closed") {
    if (getOperationPlan()->getProposed())
      throw DataException(
          "OperationPlanResource status change to closed while OperationPlan "
          "is proposed");
    setClosed(true);
  } else
    throw DataException("invalid operationplanresource status:" + s);
}

void LoadPlan::update() {
  // Update the timeline data structure
  getResource()->getLoadPlans().update(this, ld->getLoadplanQuantity(this),
                                       ld->getLoadplanDate(this));

  // Mark the operation and resource as being changed. This will trigger
  // the recomputation of their problems
  getResource()->setChanged();
  ld->getOperation()->setChanged();
}

SetupEvent* LoadPlan::getSetup(bool myself_only) const {
  auto opplan = getOperationPlan();
  if (!getResource()->getSetupMatrix() || !opplan) return nullptr;
  if (myself_only) return opplan->getSetupEvent();
  Resource::loadplanlist::const_iterator tmp;
  if (opplan->getSetupEvent())
    // Setup event being used
    tmp = opplan->getSetupEvent();
  else if (isStart())
    // Start loadplan
    tmp = this;
  else
    // End loadplan
    tmp = getOtherLoadPlan();
  while (tmp != getResource()->getLoadPlans().end()) {
    if (tmp->getEventType() == 5 &&
        (tmp->getDate() < opplan->getSetupEnd() ||
         (tmp->getOperationPlan() && tmp->getDate() == opplan->getSetupEnd() &&
          *(tmp->getOperationPlan()) < *opplan)))
      return const_cast<SetupEvent*>(static_cast<const SetupEvent*>(&*tmp));
    --tmp;
  }
  return nullptr;
}

LoadPlan::~LoadPlan() {
  getResource()->setChanged();
  getResource()->loadplans.erase(this);
}

void LoadPlan::setLoad(Load* newld) {
  // No change
  if (newld == ld) return;

  // Verify the data
  if (!newld) throw DataException("Can't switch to nullptr load");
  if (ld && ld->getOperation() != newld->getOperation())
    throw DataException(
        "Only switching to a load on the same operation is allowed");
  if (ld && ld->getResource()->hasType<ResourceBuckets>() !=
                newld->getResource()->hasType<ResourceBuckets>())
    throw DataException(
        "Cannot switch between alternate loads from bucketized and default "
        "resources");

  // Update the load and resource fields
  LoadPlan* o = getOtherLoadPlan();
  if (o) o->ld = newld;
  ld = newld;
  setResource(newld->getResource(), false, false);
}

Object* LoadPlan::reader(const MetaClass* cat, const DataValueDict& in,
                         CommandManager* mgr) {
  // Pick up the operationplan attribute. An error is reported if it's missing.
  const DataValue* opplanElement = in.get(Tags::operationplan);
  if (!opplanElement) throw DataException("Missing operationplan field");
  Object* opplanobject = opplanElement->getObject();
  if (!opplanobject || !opplanobject->hasType<OperationPlan>())
    throw DataException("Invalid operationplan field");
  OperationPlan* opplan = static_cast<OperationPlan*>(opplanobject);

  // Pick up the resource.
  const DataValue* resourceElement = in.get(Tags::resource);
  if (!resourceElement) throw DataException("Missing resource field");
  Object* resourceobject = resourceElement->getObject();
  if (!resourceobject ||
      resourceobject->getType().category != Resource::metadata)
    throw DataException("Invalid resource field");
  Resource* res = static_cast<Resource*>(resourceobject);

  // Find the load on the operationplan that has the same top resource.
  // If multiple exist, we pick up the first one.
  // If none is found, we throw a data error.
  auto ldplniter = opplan->getLoadPlans();
  LoadPlan* ldpln;
  while ((ldpln = ldplniter.next())) {
    if (ldpln->getResource()->getTop() == res->getTop()) {
      ldpln->setResource(res);
      const DataValue* statusElement = in.get(Tags::status);
      if (statusElement) ldpln->setStatus(statusElement->getString());
      return ldpln;
    }
  }
  return nullptr;
}

PyObject* LoadPlan::create(PyTypeObject* pytype, PyObject* args,
                           PyObject* kwds) {
  try {
    // Pick up the operationplan attribute. An error is reported if it's
    // missing.
    PyObject* opplanobject = PyDict_GetItemString(kwds, "operationplan");
    if (!opplanobject) throw DataException("Missing operationplan field");
    if (!PyObject_TypeCheck(opplanobject, OperationPlan::metadata->pythonClass))
      throw DataException("Invalid operationplan field");
    OperationPlan* opplan = static_cast<OperationPlan*>(opplanobject);

    // Pick up the resource.
    PyObject* resobject = PyDict_GetItemString(kwds, "resource");
    if (!resobject) throw DataException("Missing resource field");
    if (!PyObject_TypeCheck(resobject, Resource::metadata->pythonClass))
      throw DataException("Invalid resource field");
    Resource* res = static_cast<Resource*>(resobject);

    // Find the load on the operationplan that has the same top resource.
    // If multiple exist, we pick up the first one.
    // If none is found, we throw a data error.
    auto ldplniter = opplan->getLoadPlans();
    LoadPlan* ldpln;
    while ((ldpln = ldplniter.next())) {
      if (ldpln->getResource()->getTop() == res->getTop()) {
        ldpln->setResource(res);
        PyObject* statusobject = PyDict_GetItemString(kwds, "status");
        if (statusobject) {
          PythonData status(statusobject);
          ldpln->setStatus(status.getString());
        }
        break;
      }
    }

    // Iterate over extra keywords, and set attributes.
    if (!ldpln) {
      Py_INCREF(Py_None);
      return Py_None;
    } else {
      PyObject *key, *value;
      Py_ssize_t pos = 0;
      while (PyDict_Next(kwds, &pos, &key, &value)) {
        PythonData field(value);
        PyObject* key_utf8 = PyUnicode_AsUTF8String(key);
        DataKeyword attr(PyBytes_AsString(key_utf8));
        Py_DECREF(key_utf8);
        if (!attr.isA(Tags::operationplan) && !attr.isA(Tags::resource) &&
            !attr.isA(Tags::action) && !attr.isA(Tags::status)) {
          const MetaFieldBase* fmeta =
              ldpln->getType().findField(attr.getHash());
          if (!fmeta && ldpln->getType().category)
            fmeta = ldpln->getType().category->findField(attr.getHash());
          if (fmeta)
            // Update the attribute
            fmeta->setField(ldpln, field);
          else
            ldpln->setProperty(attr.getName(), value);
          ;
        }
      };
      Py_INCREF(ldpln);
      return static_cast<PyObject*>(ldpln);
    }

  } catch (...) {
    PythonType::evalException();
    return nullptr;
  }
}

double Load::getLoadplanQuantity(const LoadPlan* lp) const {
  if ((!lp->getOperationPlan()->getProposed() &&
       !lp->getOperationPlan()->getConsumeCapacity()) ||
      !lp->getOperationPlan()->getQuantity() ||
      lp->getOperationPlan()->getClosed() ||
      lp->getOperationPlan()->getCompleted())
    // No capacity consumption required
    return 0.0;
  if (!lp->getOperationPlan()->getDates().overlap(getEffective()) &&
      (lp->getOperationPlan()->getDates().getDuration() ||
       !getEffective().within(lp->getOperationPlan()->getStart())))
    // Load is not effective during this time.
    // The extra check is required to make sure that zero duration
    // operationplans operationplans don't get resized to 0
    return 0.0;
  if (lp->getResource()->hasType<ResourceBuckets>()) {
    // Bucketized resource
    auto efficiency =
        (lp->getResource()->getEfficiencyCalendar()
             ? lp->getResource()->getEfficiencyCalendar()->getValue(
                   lp->getDate())
             : lp->getResource()->getEfficiency()) /
        100.0;
    if (efficiency > 0.0)
      return -(getQuantityFixed() +
               getQuantity() * lp->getOperationPlan()->getQuantity()) /
             efficiency;
    else
      return DBL_MIN;
  } else
    // Continuous resource
    return lp->isStart() ? getQuantity() : -getQuantity();
}

tuple<double, Date, double> LoadPlan::getBucketEnd() const {
  assert(getResource()->hasType<ResourceBuckets>());
  double available_before = getOnhand();
  for (auto cur = res->getLoadPlans().begin(this);
       cur != res->getLoadPlans().end(); ++cur) {
    if (cur->getEventType() == 2)
      return make_tuple(available_before, cur->getDate(), cur->getOnhand());
    available_before = cur->getOnhand();
  }
  return make_tuple(available_before, Date::infiniteFuture, 0);
}

tuple<double, Date, double> LoadPlan::getBucketStart() const {
  assert(getResource()->hasType<ResourceBuckets>());
  double available_after = getOnhand();
  for (auto cur = res->getLoadPlans().begin(this);
       cur != res->getLoadPlans().end(); --cur) {
    available_after = cur->getQuantity();
    if (cur->getEventType() == 2) {
      auto tmp = cur->getDate();
      --cur;
      return make_tuple(
          cur != res->getLoadPlans().end() ? cur->getOnhand() : 0.0, tmp,
          available_after);
    }
  }
  return make_tuple(0.0, Date::infinitePast, available_after);
}

int LoadPlanIterator::initialize() {
  // Initialize the type
  PythonType& x = PythonExtension<LoadPlanIterator>::getPythonType();
  x.setName("loadplanIterator");
  x.setDoc("frePPLe iterator for loadplan");
  x.supportiter();
  return x.typeReady();
}

PyObject* LoadPlanIterator::iternext() {
  LoadPlan* ld;
  if (resource_or_opplan) {
    // Skip zero quantity loadplans
    while (*resiter != res->getLoadPlans().end() &&
           (*resiter)->getQuantity() == 0.0)
      ++(*resiter);
    if (*resiter == res->getLoadPlans().end()) return nullptr;

    // Return result
    ld = const_cast<LoadPlan*>(static_cast<const LoadPlan*>(&*((*resiter)++)));
  } else {
    while (*opplaniter != opplan->endLoadPlans() &&
           (*opplaniter)->getQuantity() == 0.0)
      ++(*opplaniter);
    if (*opplaniter == opplan->endLoadPlans()) return nullptr;
    ld = static_cast<LoadPlan*>(&*((*opplaniter)++));
  }
  Py_INCREF(ld);
  return const_cast<LoadPlan*>(ld);
}

LoadPlan::AlternateIterator::AlternateIterator(const LoadPlan* o) : ldplan(o) {
  if (ldplan->getLoad() && ldplan->getLoad()->getResource()->isGroup()) {
    for (Resource::memberRecursiveIterator i(ldplan->getLoad()->getResource());
         !i.empty(); ++i) {
      if (ldplan->getResource() == &*i || i->isGroup()) continue;
      Skill* sk = ldplan->getLoad()->getSkill();
      if (!sk || i->hasSkill(sk, ldplan->getDate(), ldplan->getDate())) {
        auto my_eff = i->getEfficiencyCalendar()
                          ? i->getEfficiencyCalendar()->getValue(
                                ldplan->getOperationPlan()->getStart())
                          : i->getEfficiency();
        if (my_eff > 0.0) resources.push_back(&*i);
      }
    }
  }
  resIter = resources.begin();
}

Resource* LoadPlan::AlternateIterator::next() {
  if (resIter == resources.end()) return nullptr;
  auto tmp = *resIter;
  ++resIter;
  return tmp;
}

}  // namespace frepple
