/**
 * Runtime data structures for loaded program.
 */

#include "types.h"
#include "trace.h"
#include "constants.h"
#include "specialsignatures.h"
#include "specialclasses.h"
#include "memory.h"
#include "threads.h"
#include "classes.h"
#include "language.h"
#include "configure.h"
#include "interpreter.h"
#include "exceptions.h"
#include "stack.h"
#include "platform_hooks.h"
#include "rconsole.h"
#include "magic.h"

#if 0
#define get_stack_object(MREC_)  ((Object *) get_ref_at ((MREC_)->numParameters - 1))
#endif

// Reliable globals:

byte* installedBinary;

ConstantRecord* constantTableBase;
byte* staticFieldsBase;
byte* entryClassesBase;
ClassRecord* classBase;
objSync *staticSyncBase;
byte* constantValuesBase;
byte *classStaticStateBase;
#if EXECUTE_FROM_FLASH
byte *classStatusBase;
#endif

// Temporary globals:

// (Gotta be careful with these; a lot of stuff
// is not reentrant because of globals like these).

// static ClassRecord *tempClassRecord;
// static MethodRecord *tempMethodRecord;

// Methods:

/**
 * Check the image at the given location to see if it is valid
 */
boolean is_valid_executable(byte *start, int len)
{
  MasterRecord *mr = (MasterRecord *)start;
  if (mr->magicNumber != MAGIC) return false;
  return true;
}


void install_binary(byte* ptr)
{
  installedBinary = ptr;

  constantTableBase = __get_constant_base();
  staticFieldsBase = __get_static_fields_base();
  entryClassesBase = __get_entry_classes_base();
  classBase = __get_class_base();
  constantValuesBase = __get_constant_values_base();
  gVMOptions = get_master_record()->runtimeOptions;
}

/**
 * @return Method record or null.
 */
MethodRecord *find_method (ClassRecord *classRecord, int methodSignature)
{
  MethodRecord* mr0 = get_method_table( classRecord);
  MethodRecord* mr = mr0 + get_method_cnt(classRecord);
  while( -- mr >= mr0)
    if( mr->signatureId == methodSignature)
      return mr;

  return null;
}

/**
 * Exceute the static initializer if required. Note that the ret address used
 * here is set such that the current instruction will be re-started when the
 * initialization completes.
 * @return An indication of how the VM should proceed
 */
int dispatch_static_initializer (ClassRecord *aRec, byte *retAddr)
{
  int state = get_init_state(aRec);
  ClassRecord *init = aRec;
  ClassRecord *super = get_class_record(init->parentClass);
  // Are we needed?
  if (state & C_INITIALIZED) return EXEC_CONTINUE;
  // We need to initialize all of the super classes first. So we find the
  // highest one that has not been initialized and deal with that. This code
  // will then be called again and we will init the next highest and so on
  // until all of the classes in the chain are done.
  for(;;)
  {
    // find first super class that has not been initialized
    while (init != super && (get_init_state(super) & C_INITIALIZED) == 0)
    {
      init = super;
      super = get_class_record(init->parentClass);
    }
    // Do we have an initilizer if so we have found our class
    if (has_clinit (init)) break;
    // no initializer so mark as now initialized
    set_init_state (init, C_INITIALIZED);
    // If we are at the start of the list we are done
    if (init == aRec) return EXEC_CONTINUE;
    // Otherwise go do it all again
    init = aRec;
  }
  state = get_init_state(init);
  // are we already initializing ?
  if (state & C_INITIALIZING)
  {
    // Is it this thread that is doing the init?
    if (get_sync(init)->threadId == currentThread->threadId)
      return EXEC_CONTINUE;
    // No so we must retry the current instruction
    curPc = retAddr;
    sleep_thread(1);
    schedule_request(REQUEST_SWITCH_THREAD);
    return EXEC_RETRY;
  }
  #if DEBUG_METHODS
  printf ("dispatch_static_initializer: has clinit: %d, %d\n",
          (int) aRec, (int) retAddr);
  #endif
  // Can we run it?
  if (!dispatch_special (find_method (init, _6clinit_7_4_5V), retAddr))
    return EXEC_RETRY;
  // Mark for next time
  set_init_state(init, C_INITIALIZING);
  // and claim the monitor
  current_stackframe()->monitor = (Object *)init;
  enter_monitor (currentThread, (Object *)init);
  return EXEC_RUN;
}

void dispatch_virtual (Object *ref, int signature, byte *retAddr)
{
  ClassRecord *classRecord;
  MethodRecord *methodRecord;
  int classIndex;

#if DEBUG_METHODS
  printf("dispatch_virtual %d\n", signature);
#endif
  if (ref == JNULL)
  {
    throw_exception (nullPointerException);
    return;
  }
  // When calling methods on arrays, we use the methods for the Object class...
  classIndex = get_class_index(ref);
 LABEL_METHODLOOKUP:
  classRecord = get_class_record (classIndex);
  methodRecord = find_method (classRecord, signature);
  if (methodRecord == null)
  {
    #if SAFE
    if (classIndex == JAVA_LANG_OBJECT)
    {
      throw_exception (noSuchMethodError);
      return;
    }
    #endif
    classIndex = classRecord->parentClass;
    goto LABEL_METHODLOOKUP;
  }

  if (dispatch_special (methodRecord, retAddr))
  {
    if (is_synchronized(methodRecord))
    {
      current_stackframe()->monitor = ref;
      enter_monitor (currentThread, ref);
    }
  }
}

/**
 * Calls static initializer if necessary before
 * dispatching with dispatch_special().
 * @param retAddr Return bytecode address.
 * @param btAddr Backtrack bytecode address (in case
 *               static initializer is executed).
 */
void dispatch_special_checked (byte classIndex, byte methodIndex,
                               byte *retAddr, byte *btAddr)
{
  ClassRecord *classRecord;
  MethodRecord *methodRecord;
  #if DEBUG_METHODS
  printf ("dispatch_special_checked: %d, %d, %d, %d\n",
          classIndex, methodIndex, (int) retAddr, (int) btAddr);
  #endif
  // If we need to run the initializer then the real method will get called
  // later, when we re-run the current instruction.
  classRecord = get_class_record (classIndex);
  if (!is_initialized_idx (classIndex))
    if (dispatch_static_initializer (classRecord, btAddr) != EXEC_CONTINUE)
      return;
  methodRecord = get_method_record (classRecord, methodIndex);
  if(dispatch_special (methodRecord, retAddr))
  {
    if (is_synchronized(methodRecord))
    {
      if (!is_static(methodRecord))
      {

        Object *ref = (Object *)curLocalsBase[0];
        current_stackframe()->monitor = ref;
        enter_monitor (currentThread, ref);
      }
      else
      {

        Object *ref = (Object *)classRecord;
        current_stackframe()->monitor = ref;
        enter_monitor (currentThread, ref);
      }
    }
  }
}

/**
 * @param classRecord Record for method class.
 * @param methodRecord Calle's method record.
 * @param retAddr What the PC should be upon return.
 * @return true iff the stack frame was pushed.
 */
boolean dispatch_special (MethodRecord *methodRecord, byte *retAddr)
{
  /**
   * Note: This code is a little tricky, particularly when used with
   * a garbage collector. It manipulates the stack frame and in some cases
   * may need to perform memory allocation. In all cases we must take care
   * to ensure that if an allocation can be made then any live objects
   * on the stack must be below the current stack pointer.
   * In addition to the above we take great care so that this function can
   * be restarted (to allow us to wait for available memory). To enable this
   * we avoid making any commitments to changes to global state until both
   * stacks have been commited.
   */
  #if DEBUG_METHODS
  int debug_ctr;
  #endif

  Object *stackFrameArray;
  StackFrame *stackFrame;
  StackFrame *stackBase;
  int newStackFrameIndex;
  STACKWORD *newStackTop;

  #if DEBUG_BYTECODE
  printf ("\n------ dispatch special - %d ------------------\n\n",
          methodRecord->signatureId);
  #endif

  #if DEBUG_METHODS
  printf ("dispatch_special: %d, %d\n", 
          (int) methodRecord, (int) retAddr);
  printf ("-- signature id = %d\n", methodRecord->signatureId);
  printf ("-- code offset  = %d\n", methodRecord->codeOffset);
  printf ("-- flags        = %d\n", methodRecord->mflags);
  printf ("-- num params   = %d\n", methodRecord->numParameters);
  printf ("-- stack ptr    = %d\n", (int) get_stack_ptr());
  printf ("-- max stack ptr= %d\n", (int) (currentThread->stackArray + (get_array_size(currentThread->stackArray))*2));
  #endif


  // First deal with the easy case of a native call...
  if (is_native (methodRecord))
  {
  #if DEBUG_METHODS
  printf ("-- native\n");
  #endif 
    // WARNING: Once the instruction below has been executed we may have
    // references on the stack that are above the stack pointer. If a GC
    // gets run when in this state the reference may get collected as
    // grabage. This means that any native functions that take a reference
    // parameter and that may end up allocating memory *MUST* protect that
    // reference before calling the allocator...
    pop_words_cur (methodRecord->numParameters);
    switch(dispatch_native (methodRecord->signatureId, get_stack_ptr_cur() + 1))
    {
      case EXEC_RETRY:
        // Need to re-start the instruction, so reset the state of the stack
        curStackTop += methodRecord->numParameters;
        break;
      case EXEC_CONTINUE:
        // Normal completion return to the requested point.
        curPc = retAddr;
        break;
      case EXEC_RUN:
        // We are running new code, curPc will be set. Nothing to do.
        break;
      case EXEC_EXCEPTION:
        // An exception has been thrown. The PC will be set correctly and
        // the stack may have been adjusted...
        break;
    }
    // Stack frame not pushed
    return false;
  }
  // Now start to build the new stack frames. We start by placing the
  // the new stack pointer below any params. The params will become locals
  // in the new frame.
  newStackTop = get_stack_ptr_cur() - methodRecord->numParameters;

  newStackFrameIndex = (int)(byte)currentThread->stackFrameArraySize;
  if (newStackFrameIndex >=  255)
  {
      throw_exception (stackOverflowError);
      return false;
  }
  #if DEBUG_METHODS
  for (debug_ctr = 0; debug_ctr < methodRecord->numParameters; debug_ctr++)
    printf ("-- param[%d]    = %ld\n", debug_ctr, (long) get_stack_ptr()[debug_ctr+1]);  
  #endif
  stackFrameArray = ref2obj(currentThread->stackFrameArray);
  stackBase = (StackFrame *)array_start(stackFrameArray);
  // Setup OLD stackframe ready for return
  stackFrame = stackBase + (newStackFrameIndex);
  stackFrame->stackTop = newStackTop;
  stackFrame->pc = retAddr;
  // Push NEW stack frame
  // Increment size of stack frame array but do not commit to it until we have
  // completely built both new stacks.
  newStackFrameIndex++;
  stackFrame++;
  if (((byte *)stackFrame - (byte *)stackBase) >= get_array_length(stackFrameArray))
  {
#if FIXED_STACK_SIZE
    throw_exception (stackOverflowError);
    return false;
#else
    int newlen = (stackFrame - stackBase)*3/2;
    // Stack frames are indexed by a byte value so limit the size. 
    if (newlen > 256)
      newlen = 256;
    // increase the stack frame size
    stackFrameArray = reallocate_array(stackFrameArray, newlen*(sizeof(StackFrame)));
    // If can't allocate new stack, give in!
    if (stackFrameArray == JNULL)
      return false;
    // Assign new array
    currentThread->stackFrameArray = ptr2ref(stackFrameArray);
    stackFrame = (StackFrame *)array_start(stackFrameArray) + newStackFrameIndex;
#endif
  }
  
  // Initialize rest of new stack frame
  stackFrame->methodRecord = methodRecord;
  stackFrame->monitor = null;
  stackFrame->localsBase = newStackTop + 1;
  // Allocate space for locals etc.
  newStackTop = init_sp(stackFrame, methodRecord);
  
  // Check for stack overflow
  // (stackTop + methodRecord->maxOperands) >= (stack_array() + STACK_SIZE);
  if (is_stack_overflow (newStackTop, methodRecord))
  {
#if FIXED_STACK_SIZE
    throw_exception (stackOverflowError);
    return false;
#else
    StackFrame *stackBase;
    byte *oldStart = array_start((Object *)(currentThread->stackArray));
    int offset;
    int i;
    
    // Need at least this many bytes
    // int len = (int)(stackTop + methodRecord->maxOperands) - (int)(stack_array()) - HEADER_SIZE;
    
    // Need to compute new array size (as distinct from number of bytes in array).
    int newlen = (((int)(newStackTop + methodRecord->maxOperands) - (int)(stack_array()) + 3) / 4) * 3 / 2;
    REFERENCE newStackArray = ptr2ref(reallocate_array(word2ptr(currentThread->stackArray), newlen));
    // If can't allocate new stack, give in!
    if (newStackArray == JNULL)
      return false;
    // Adjust pointers.
    offset = array_start((Object *)newStackArray) - oldStart;
    stackBase = (StackFrame *)array_start(stackFrameArray);
    newStackTop = word2ptr(ptr2word(newStackTop) + offset);
    curLocalsBase = word2ptr(ptr2word(curLocalsBase) + offset);
#if DEBUG_MEMORY
    printf("thread=%d, stackTop(%d), localsBase(%d)=%d\n", currentThread->threadId, (int)stackTop, (int)localsBase, (int)(*localsBase));
#endif
    for (i=newStackFrameIndex;
         i >= 0;
         i--)
    {
      stackBase[i].localsBase = word2ptr(ptr2word(stackBase[i].localsBase) + offset);
      stackBase[i].stackTop = word2ptr(ptr2word(stackBase[i].stackTop) + offset);
#if DEBUG_MEMORY
      printf("stackBase[%d].localsBase(%d) = %d\n", i, (int)stackBase[i].localsBase, (int)(*stackBase[i].localsBase));
#endif
    }
    // Assign new array
    currentThread->stackArray = newStackArray;
#endif
  }
  // All set. So now we can finally commit to the new stack frames
  currentThread->stackFrameArraySize = newStackFrameIndex;
  update_constant_registers (stackFrame);
  curStackTop = newStackTop;
  // and jump to the start of the new code
  curPc = get_code_ptr(methodRecord);
  return true;
}

/**
 * Perform a return from a method call. Clean up the stack, setup
 * the return of any results, release any monitor and finally set the
 * PC for the return address.
 */
void do_return (int numWords)
{
  StackFrame *stackFrame;
  STACKWORD *fromStackPtr;

  stackFrame = current_stackframe();

  #if DEBUG_BYTECODE
  printf ("\n------ return ----- %d ------------------\n\n",
          stackFrame->methodRecord->signatureId);
  #endif

  #if DEBUG_METHODS
  printf ("do_return: method: %d  #  num. words: %d\n", 
          stackFrame->methodRecord->signatureId, numWords);
  #endif

  #ifdef VERIFY
  assert (stackFrame != null, LANGUAGE3);
  #endif
  if (stackFrame->monitor != null)
  {
    // if this was a class init then mark the class as now complete
    if (stackFrame->methodRecord->signatureId ==  _6clinit_7_4_5V)
      set_init_state((ClassRecord *)(stackFrame->monitor), C_INITIALIZED);
    exit_monitor (currentThread, stackFrame->monitor);
  }

  #if DEBUG_THREADS || DEBUG_METHODS
  printf ("do_return: stack frame array size: %d\n", currentThread->stackFrameArraySize);
  #endif

  if (currentThread->stackFrameArraySize == 1)
  {
    #if DEBUG_METHODS
    printf ("do_return: thread is done: %d\n", (int) currentThread);
    #endif
    currentThread->state = DEAD;
    schedule_request (REQUEST_SWITCH_THREAD);
    return;
  }

  // Place source ptr below data to be copied up the stack
  fromStackPtr = get_stack_ptr_at_cur(numWords);
  // Pop stack frame
  currentThread->stackFrameArraySize--;
  stackFrame--;
  // Assign registers
  update_registers (stackFrame);
  #if DEBUG_METHODS
  printf ("do_return: stack reset to:\n");
  printf ("-- stack ptr = %d\n", (int) get_stack_ptr());
  #endif

  while (numWords--)
  {
    push_word_cur (*(++fromStackPtr));
  }  
}

/**
 * Exceute a "program" on the current thread.
 * NOTE: This call, resets both stacks and so the called method
 * will never return. The thread will exit.
 * @return An indication of how the VM should proceed.
 */
int execute_program(int prog)
{
  // Now find the class
  ClassRecord *classRecord = get_class_record (get_entry_class (prog));
  MethodRecord *mRec = find_method (classRecord, main_4_1Ljava_3lang_3String_2_5V);
  // smash the stacks back to the initial state
  currentThread->stackFrameArraySize = 0;
  init_sp_pv();
  update_stack_frame(current_stackframe());
  // Push the param
  set_top_ref_cur(JNULL);
  // Push stack frame for main method:
  dispatch_special (mRec, null);
  // and for static initializer
  dispatch_static_initializer (classRecord, curPc);
  return EXEC_RUN;
}


/**
 * Check to see if obj is a sub type of the type described by
 * cls. 
 */
static boolean sub_type_of(byte obj, const byte cls)
{
  if (cls == JAVA_LANG_OBJECT) return true;
  while (obj != cls)
  {
    obj = get_class_record(obj)->parentClass;
    if (obj == JAVA_LANG_OBJECT) return false;
  }
  return true;
}
  

/**
 * Check to see if obj is an instance of the type described by sig.
 * @return true or false
 */
boolean instance_of (Object *obj, const byte cls)
{
  byte rtCls;

  if (obj == null)
    return false;
  
  // Check for common cases
  if (cls == JAVA_LANG_OBJECT) return true;
  rtCls = get_class_index(obj);
  if (rtCls == cls) return true;
  return is_assignable(rtCls, cls);
}

/**
 * Check to see if it is allowed to assign the an object of type srcCls
 * to an object of type dstCls.
 */
boolean is_assignable(const byte srcCls, const byte dstCls)
{
  ClassRecord *dstRec;
  // Check common cases
  if (srcCls == dstCls || dstCls == JAVA_LANG_OBJECT) return true;
  dstRec = get_class_record(dstCls);
  if (is_interface(dstRec))
  {
    // we are testing against an interface. So we use the associated interface
    // map to test if the src implements it...
    int base = get_interface_map_base(dstRec);
    // Special case all arrays implement cloneable
    if (dstCls == JAVA_LANG_CLONEABLE && is_array_class(get_class_record(srcCls))) return true;
    if (srcCls < base) return false;
    if (srcCls - base >= get_interface_map_len(dstRec)) return false;
    base = srcCls - base;
    return ((get_interface_map(dstRec)[base/8]) & (1 << (base%8))) != 0;
  }
  return sub_type_of(srcCls, dstCls);
}






